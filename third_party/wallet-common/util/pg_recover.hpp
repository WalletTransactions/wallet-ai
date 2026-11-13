// Восстановление соединения libpq после рестарта/обрыва Postgres — общее для
// всех сервисов с собственной обёрткой Database.
//
// Почему не хватает `if (PQstatus(conn) != CONNECTION_OK) PQreset(conn)`:
// PQstatus не знает, что сокет умер, пока клиент не попробует в него
// писать/читать. После рестарта БД статус остаётся CONNECTION_OK, и первый
// запрос падает с «server closed the connection unexpectedly».
//
// Три слоя защиты:
//   1. revive()  — перед каждым запросом: неблокирующая проверка сокета
//      (FIN/RST от сервера уже лежат в буфере ядра), без round-trip.
//   2. exec_params() — если соединение умерло прямо во время запроса: сброс
//      соединения, а для чтения вне транзакции ещё и один повтор.
//   3. Keepalive — раз в минуту проверка живого соединения, пока сервис
//      простаивает.
//
// Запись автоматически НЕ повторяется: клиент не знает, выполнил ли сервер
// запрос до обрыва, и повтор мог бы дублировать операцию. Внутри транзакции
// PQreset не вызывается вовсе — он молча потерял бы BEGIN.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;
}

namespace pg_recover {

// Период фоновой проверки по умолчанию.
constexpr std::chrono::seconds kDefaultKeepaliveInterval{60};

enum class Health {
    Ok,          // соединение живо, ничего не делали
    Reconnected, // было мёртвым, переоткрыто (PQreset)
    Failed,      // было мёртвым, переоткрыть не удалось (error заполнен)
};

// true, если по сокету видно, что сервер соединение закрыл (EOF/ошибка).
// Дешёвая: poll без ожидания, round-trip нет. Читает накопившиеся данные
// через PQconsumeInput.
bool socket_dead(PGconn* conn);

// Перед запросом: PQstatus + socket_dead; при смерти — PQreset. После
// Reconnected вызывающий заново вешает то, что PQreset сбрасывает
// (PQsetNoticeProcessor и т.п.). Вне транзакции.
Health revive(PGconn* conn, std::string* error = nullptr);

// Фоновая проверка: SELECT 1 (round-trip) и PQreset при неудаче.
Health probe(PGconn* conn, std::string* error = nullptr);

// Соединение потеряно (после неудачного запроса): PQstatus != CONNECTION_OK.
bool connection_lost(PGconn* conn);

// Только чтение: начинается с SELECT/WITH и не содержит слов
// INSERT/UPDATE/DELETE/MERGE/nextval/setval/... (слово целиком, без учёта
// регистра). Ложное «не только чтение» безопасно — теряется лишь повтор.
bool is_read_only(const std::string& sql);

// PQexecParams с восстановлением. in_transaction — вызов из открытой
// транзакции: PQreset и повтор запрещены. Вернуть результат (или nullptr при
// потерянном соединении) — как PQexecParams; вызывающий обрабатывает ошибку
// как раньше. reconnected (необязательный) выставляется в true, если
// соединение переоткрывали (повесить NoticeProcessor заново).
PGresult* exec_params(PGconn* conn, const std::string& sql, int n_params, const char* const* values,
                      bool in_transaction, bool* reconnected = nullptr);

// Фоновый поток: раз в interval вызывает tick(). Останавливается в
// деструкторе без ожидания полного интервала.
class Keepalive {
public:
    Keepalive(std::function<void()> tick, std::chrono::seconds interval = kDefaultKeepaliveInterval);
    ~Keepalive();
    Keepalive(const Keepalive&) = delete;
    Keepalive& operator=(const Keepalive&) = delete;

private:
    std::function<void()> tick_;
    std::chrono::seconds interval_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace pg_recover
