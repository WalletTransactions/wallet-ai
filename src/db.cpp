#include "db.hpp"

#include <chrono>
#include <stdexcept>

#include <libpq-fe.h>

#include "nlohmann/json.hpp"
#include "util/log.hpp"

namespace {

// Встроенный набор имён параметров, чьи значения ВСЕГДА маскируются в логе
// (см. mask_param_names_/exec() в db.hpp), даже когда log_params_ включён —
// работает, только если вызывающий код указал это имя параметру через
// param_names. Общие секреты, а не то, что реально хранится в этой БД сейчас
// (в wallets секретов нет вовсе) — задел на будущее, чтобы решение о
// маскировке не принималось заново в каждом месте, которое такую колонку
// когда-нибудь заведёт.
const std::unordered_set<std::string>& builtin_mask_param_names() {
    static const std::unordered_set<std::string> names = {
        "password", "client_secret", "refresh_token", "access_token", "id_token", "token", "secret",
    };
    return names;
}

} // namespace

namespace db {

Database::Database(const std::string& conninfo, bool log_params,
                    const std::unordered_set<std::string>& mask_param_names)
    : log_params_(log_params), mask_param_names_(builtin_mask_param_names()) {
    mask_param_names_.insert(mask_param_names.begin(), mask_param_names.end());
    conn_ = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn_);
        PQfinish(conn_);
        conn_ = nullptr;
        throw std::runtime_error("не удалось подключиться к Postgres: " + err);
    }
}

Database::~Database() {
    if (conn_ != nullptr) PQfinish(conn_);
}

void Database::verify_schema() {
    // Роль сервиса лишена DDL (см. infra/db/init-db.sh) — схема живёт в
    // миграциях wallet-ai/migrations/, этот метод только проверяет, что они
    // накачены: иначе импорт выключается сразу, а не падает на первом запросе.
    for (const char* table : {"statement_import", "statement_chunk", "statement_row", "merchant_rule",
                              "statement_imported", "format_profile", "format_example"}) {
        auto res = exec("SELECT to_regclass('public." + std::string(table) + "')::text");
        if (res.rows.empty() || res.rows[0].fields.at(0).empty()) {
            throw std::runtime_error(
                "таблица '" + std::string(table) + "' не найдена — схема ещё не накачена. "
                "Запустите infra/db/migrate.sh ai up (см. infra/db/README.md, \"Миграции\").");
        }
    }
}

Result Database::exec(const std::string& sql, const std::vector<std::string>& params,
                       const std::vector<std::string>& param_names) {
    std::lock_guard<std::mutex> lock(mutex_);
    return exec_locked(sql, params, param_names);
}

Result Database::exec_locked(const std::string& sql, const std::vector<std::string>& params,
                              const std::vector<std::string>& param_names) {
    // Реальный случай (2026-09-17, test-wallet-report-loadtest #15): одно из
    // N соединений пула умерло без единого сообщения об ошибке (PQexecParams
    // на мёртвом conn_ вернул результат с пустым PQresultErrorMessage — не
    // "server closed the connection unexpectedly", как в похожем случае у
    // wallet-api, а вовсе без текста) — и с этого момента каждый acquire(),
    // которому доставался именно этот слот пула, ронял ВСЕ report-эндпоинты
    // разом. Тот же фикс, что уже стоит в wallet-api/google-auth (см. их
    // db.cpp): PQreset() переоткрывает соединение теми же параметрами, что
    // были у PQconnectdb() при создании этой Database.
    if (PQstatus(conn_) != CONNECTION_OK) {
        applog::warn("db connection is down, reconnecting");
        PQreset(conn_);
        if (PQstatus(conn_) != CONNECTION_OK) {
            std::string err = PQerrorMessage(conn_);
            applog::error("db reconnect failed", {{"event", "db_reconnect_failed"}, {"error", err}});
            throw std::runtime_error("Postgres: соединение потеряно, переподключиться не удалось: " + err);
        }
        applog::info("db reconnected", {{"event", "db_reconnected"}});
    }

    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) values.push_back(p.c_str());

    // steady_clock — для длительности, а не system_clock: последний может
    // скакнуть назад (NTP-коррекция, перевод времени), тогда latency ушла бы
    // в отрицательную. Мьютекс уже захвачен выше — таймер меряет время
    // именно самого PQexecParams, а не ожидания на mutex_ перед ним (тут
    // конкуренция за одно соединение и так означала бы, что latency в логе
    // одного запроса включала бы чужое время выполнения — вводило бы в
    // заблуждение).
    auto started = std::chrono::steady_clock::now();
    PGresult* res = PQexecParams(conn_, sql.c_str(), static_cast<int>(values.size()),
                                  nullptr /* типы параметров — пусть Postgres выведет сам */,
                                  values.data(), nullptr, nullptr, 0 /* текстовый формат */);
    double duration_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();

    // sql — безопасно логировать всегда: это текст с $1/$2/... плейсхолдерами
    // (PQexecParams, не конкатенация строк), литеральных значений в нём нет.
    // Сами params — потенциально чувствительные данные приложения (email,
    // суммы и т.п.) — попадают в лог, только если явно попросили (см.
    // log_params_ в db.hpp); из них те, чьё имя (param_names, по позиции)
    // входит в mask_param_names_, всё равно маскируются "***" — секрет не
    // должен осесть в логе в открытом виде, даже когда параметры логируются
    // намеренно для отладки (см. db.hpp).
    // "event" здесь — та же машиночитаемая метка, что и "request_finished"
    // на строке set_logger (см. main.cpp) — отличает СТРОКУ ПРО ДЛИТЕЛЬНОСТЬ
    // ОДНОГО SQL-ВЫЗОВА от строки про длительность ВСЕГО запроса целиком:
    // у обеих есть duration_ms, но это разные величины, агрегировать их
    // вместе в Dashboards без разбора msg (см. main.cpp) нельзя.
    nlohmann::json details = {{"event", "db_query"}, {"sql", sql}, {"duration_ms", duration_ms}};
    if (log_params_) {
        nlohmann::json logged = nlohmann::json::array();
        for (std::size_t i = 0; i < params.size(); ++i) {
            bool masked = i < param_names.size() && !param_names[i].empty() &&
                          mask_param_names_.count(param_names[i]) != 0;
            logged.push_back(masked ? std::string("***") : params[i]);
        }
        details["params"] = std::move(logged);
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        std::string err = PQresultErrorMessage(res);
        // Готовые, отдельные поля вместо разбора текста ошибки (см. QueryError
        // в db.hpp, зачем) — оба могут быть nullptr, если Postgres не заполнил
        // их для этого класса ошибки (не всякая ошибка — про constraint).
        const char* sqlstate_field = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        const char* constraint_field = PQresultErrorField(res, PG_DIAG_CONSTRAINT_NAME);
        std::string sqlstate = sqlstate_field != nullptr ? sqlstate_field : "";
        std::string constraint_name = constraint_field != nullptr ? constraint_field : "";
        PQclear(res);
        details["event"] = "db_query_failed";
        details["error"] = err;
        details["sqlstate"] = sqlstate;
        applog::error("db query failed", details);
        throw QueryError("Postgres: " + err, sqlstate, constraint_name);
    }

    Result out;
    int nrows = PQntuples(res);
    int ncols = PQnfields(res);
    out.rows.reserve(static_cast<size_t>(nrows));
    for (int r = 0; r < nrows; ++r) {
        Row row;
        row.fields.reserve(static_cast<size_t>(ncols));
        for (int c = 0; c < ncols; ++c) {
            row.fields.emplace_back(PQgetisnull(res, r, c) != 0 ? std::string{}
                                                                 : std::string(PQgetvalue(res, r, c)));
        }
        out.rows.push_back(std::move(row));
    }
    PQclear(res);

    // rows — сколько строк ВЕРНУЛОСЬ (PQntuples): для SELECT и для
    // INSERT/UPDATE/DELETE ... RETURNING (весь repo.cpp написан именно так)
    // это и есть содержательное число. Для гипотетического будущего запроса
    // без RETURNING здесь было бы 0 даже при реально изменённых строках
    // (PQcmdTuples() показал бы их) — сейчас в кодовой базе таких нет, так
    // что не усложняем ради случая, которого не существует.
    details["rows"] = out.rows.size();
    // debug, не info — строка на КАЖДЫЙ SQL-вызов: сев нагрузочного контура
    // (seed_loadtest.cpp, тот же db.cpp) печатал её на каждый INSERT прямо в
    // консоль Jenkins — 7 ГБ лога за один билд. Отключается LOG_LEVEL=info
    // (см. log.hpp); db_query_failed выше — по-прежнему error.
    if (should_log_query(out.rows.size())) applog::debug("db query", details);

    return out;
}

Database::Transaction::Transaction(Database& db) : db_(db), lock_(db.mutex_) {
    db_.exec_locked("BEGIN");
}

Database::Transaction::~Transaction() {
    if (committed_) return;
    try {
        db_.exec_locked("ROLLBACK");
    } catch (const std::exception& ex) {
        // Деструктор не имеет права бросать — если соединение уже развалилось
        // (та же причина, по которой упал предыдущий exec() в этой
        // транзакции), ROLLBACK тоже не пройдёт; просто фиксируем в лог.
        applog::error(std::string("ROLLBACK failed: ") + ex.what());
    }
}

Result Database::Transaction::exec(const std::string& sql, const std::vector<std::string>& params,
                                    const std::vector<std::string>& param_names) {
    return db_.exec_locked(sql, params, param_names);
}

void Database::Transaction::commit() {
    db_.exec_locked("COMMIT");
    committed_ = true;
}

Pool::Pool(const std::string& conninfo, std::size_t size, bool log_params,
           const std::unordered_set<std::string>& mask_param_names) {
    if (size == 0) throw std::runtime_error("db::Pool: size должен быть хотя бы 1");
    conns_.reserve(size);
    // Каждая Database сама бросает std::runtime_error при неудаче (см. её
    // конструктор) — падаем здесь же, при старте процесса (main.cpp), а не
    // тихо остаёмся с пулом меньше запрошенного размера.
    for (std::size_t i = 0; i < size; ++i) {
        conns_.push_back(std::make_unique<Database>(conninfo, log_params, mask_param_names));
    }
    idle_.reserve(size);
    for (auto& c : conns_) idle_.push_back(c.get());
}

Pool::Lease::Lease(Lease&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
    other.pool_ = nullptr;
    other.conn_ = nullptr;
}

Pool::Lease::~Lease() {
    // pool_ == nullptr — из этого Lease уже переместили (см. move-конструктор
    // выше): ему возвращать соединение в пул не нужно, это сделает тот Lease,
    // в который его переместили.
    if (pool_ != nullptr) pool_->release(conn_);
}

Pool::Lease Pool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !idle_.empty(); });
    Database* conn = idle_.back();
    idle_.pop_back();
    return Lease(this, conn);
}

void Pool::release(Database* conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_.push_back(conn);
    }
    // notify_one, не _all — ровно одно соединение освободилось, будить всех
    // ожидающих acquire() ради него было бы лишней работой (проснулись бы,
    // проверили условие, все кроме одного заснули бы обратно).
    cv_.notify_one();
}

namespace {
thread_local bool t_quiet_empty = false;
} // namespace

QuietEmptyResults::QuietEmptyResults() : prev_(t_quiet_empty) { t_quiet_empty = true; }
QuietEmptyResults::~QuietEmptyResults() { t_quiet_empty = prev_; }

bool should_log_query(std::size_t rows) { return rows != 0 || !t_quiet_empty; }

} // namespace db
