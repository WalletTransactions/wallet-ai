#include "util/pg_recover.hpp"

#include <libpq-fe.h>
#include <poll.h>

#include <cctype>
#include <exception>
#include <unordered_set>

#include "util/log.hpp"
#include "util/metrics.hpp"

namespace pg_recover {

namespace {

// Переоткрыть соединение и записать в журнал/метрику, почему. reason — только
// литералы из кода (кардинальность метрики).
Health reset(PGconn* conn, const char* reason, std::string* error) {
    applog::warn("db connection is down, reconnecting", {{"event", "db_reconnecting"}, {"reason", reason}});
    PQreset(conn);
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn);
        applog::error("db reconnect failed", {{"event", "db_reconnect_failed"}, {"reason", reason}, {"error", err}});
        if (error != nullptr) *error = err;
        return Health::Failed;
    }
    metrics::inc_counter("db_reconnect_total", "Переподключения к Postgres по причине.", "reason", reason);
    applog::info("db reconnected", {{"event", "db_reconnected"}, {"reason", reason}});
    return Health::Reconnected;
}

bool is_word_char(unsigned char c) { return std::isalnum(c) != 0 || c == '_'; }

} // namespace

bool socket_dead(PGconn* conn) {
    int fd = PQsocket(conn);
    if (fd < 0) return true;
    // Сервер при остановке/terminate_backend сначала присылает FATAL-сообщение
    // («terminating connection…»), и только следующим чтением виден EOF.
    // Поэтому читаем, пока в сокете что-то есть (с запасом по числу витков).
    for (int i = 0; i < 8; ++i) {
        pollfd p{fd, POLLIN, 0};
        int ready = ::poll(&p, 1, 0);
        if (ready <= 0) return false; // ничего не пришло (или EINTR): соединение считаем живым
        if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return true;
        if ((p.revents & POLLIN) == 0) return false;
        // Либо EOF от сервера (рестарт/обрыв), либо асинхронные сообщения
        // (NOTICE, FATAL). PQconsumeInput на EOF вернёт 0 и пометит
        // соединение BAD. Работает и поверх SSL, в отличие от recv(MSG_PEEK).
        if (PQconsumeInput(conn) == 0) return true;
        if (PQstatus(conn) != CONNECTION_OK) return true;
    }
    return false;
}

Health revive(PGconn* conn, std::string* error) {
    if (PQstatus(conn) != CONNECTION_OK) return reset(conn, "status", error);
    if (socket_dead(conn)) return reset(conn, "socket", error);
    return Health::Ok;
}

Health probe(PGconn* conn, std::string* error) {
    if (PQstatus(conn) != CONNECTION_OK) return reset(conn, "status", error);
    PGresult* res = PQexec(conn, "SELECT 1");
    bool ok = res != nullptr && PQresultStatus(res) == PGRES_TUPLES_OK;
    PQclear(res);
    if (ok) return Health::Ok;
    return reset(conn, "probe", error);
}

bool connection_lost(PGconn* conn) { return PQstatus(conn) != CONNECTION_OK; }

bool is_read_only(const std::string& sql) {
    static const std::unordered_set<std::string> kWriting = {
        "insert", "update", "delete", "merge",  "nextval", "setval", "create", "drop",
        "alter",  "truncate", "copy",  "call",   "do",      "lock",   "grant",  "revoke",
        "vacuum", "refresh",  "reindex",
    };
    std::size_t i = 0;
    bool first = true;
    while (i < sql.size()) {
        unsigned char c = static_cast<unsigned char>(sql[i]);
        if (!is_word_char(c)) {
            ++i;
            continue;
        }
        std::size_t start = i;
        while (i < sql.size() && is_word_char(static_cast<unsigned char>(sql[i]))) ++i;
        std::string word = sql.substr(start, i - start);
        for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (first) {
            if (word != "select" && word != "with") return false;
            first = false;
        }
        if (kWriting.count(word) != 0) return false;
    }
    return !first; // пустой запрос — не «чтение»
}

PGresult* exec_params(PGconn* conn, const std::string& sql, int n_params, const char* const* values,
                      bool in_transaction, bool* reconnected) {
    if (reconnected != nullptr) *reconnected = false;
    if (!in_transaction) {
        std::string error;
        Health h = revive(conn, &error);
        if (h == Health::Failed) return nullptr; // причина — в PQerrorMessage(conn), как после PQexecParams
        if (h == Health::Reconnected && reconnected != nullptr) *reconnected = true;
    }

    auto run = [&] {
        return PQexecParams(conn, sql.c_str(), n_params, nullptr, values, nullptr, nullptr, 0);
    };
    PGresult* res = run();
    if (!connection_lost(conn)) return res; // успех или обычная SQL-ошибка

    // Соединение умерло во время запроса. Запись не повторяем (неизвестно,
    // выполнил ли её сервер), в транзакции PQreset потерял бы BEGIN —
    // оставляем соединение как есть: PQerrorMessage хранит причину, а
    // следующий revive() его переоткроет.
    if (in_transaction || !is_read_only(sql)) return res;

    PQclear(res);
    std::string error;
    Health h = reset(conn, "retry", &error);
    if (h == Health::Failed) return nullptr;
    if (reconnected != nullptr) *reconnected = true;
    return run();
}

Keepalive::Keepalive(std::function<void()> tick, std::chrono::seconds interval)
    : tick_(std::move(tick)), interval_(interval) {
    thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!cv_.wait_for(lock, interval_, [this] { return stop_; })) {
            lock.unlock();
            try {
                tick_();
            } catch (const std::exception& ex) {
                applog::error(std::string("db keepalive failed: ") + ex.what());
            }
            lock.lock();
        }
    });
}

Keepalive::~Keepalive() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

} // namespace pg_recover
