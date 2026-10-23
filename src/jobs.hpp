// Асинхронные задания к LLM (org/docs/TODO_wallet_ai.md, §3–§5): клиент
// создаёт задание и получает результат long-poll'ом, модель думает столько,
// сколько нужно виду задания. Хранилище в памяти процесса: рестарт теряет
// задания — допустимо (пользователь напишет ещё раз), база/Redis — со второй
// репликой.
//
// Store потокобезопасен: хендлеры httplib создают и читают задания, воркеры
// (по одному на одновременный запрос к GPU) берут и завершают.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

namespace jobs {

enum class Status { Queued, Running, Done, Failed, Cancelled };
const char* to_string(Status s);
bool is_final(Status s);

// Интерактивные (трата текстом, чат) воркер берёт раньше фоновых (разметка
// импорта): трата пользователя не ждёт пачку выписки.
enum class Priority { Interactive, Background };

using Clock = std::chrono::steady_clock;

struct Job {
    std::string id; // UUID v4
    std::string user_id;
    std::string kind;
    Priority priority = Priority::Interactive;
    std::string idempotency_key;
    // Вход живёт до завершения задания, результат — до TTL: в них тексты
    // трат (приватность, §5).
    nlohmann::json input;
    nlohmann::json result;   // при Done
    std::string failure;     // при Failed: timeout, unavailable, ...
    Status status = Status::Queued;
    bool cancel_requested = false; // DELETE во время Running: результат выбросим
    // Задание импорта выписки (extract_rows, classify_rows) — создаёт сам
    // сервис, в лимитах пользователя не считается (спека ai-jobs).
    bool internal = false;
    Clock::time_point created;
    Clock::time_point started;
    Clock::time_point finished;
};

struct Limits {
    std::size_t max_queued_interactive = 20; // больше — 429 busy
    std::size_t max_active_per_user = 3;     // незавершённых заданий
    int max_per_minute = 10;                 // новых заданий на пользователя
    std::chrono::seconds ttl{600};           // сколько хранить завершённое
};

enum class CreateError { None, Busy, RateLimited, TooManyActive };
const char* to_string(CreateError e);

struct Created {
    CreateError error = CreateError::None;
    Job job;            // при None — снимок созданного (или найденного) задания
    bool reused = false; // тот же idempotency_key — то же задание, без нового запуска
};

class Store {
public:
    using NowFn = std::function<Clock::time_point()>;

    explicit Store(Limits limits, NowFn now = [] { return Clock::now(); });

    // idempotency_key — пусто, если не нужен. Повтор с тем же ключом от того
    // же пользователя возвращает существующее задание (лимиты не трогает).
    Created create(const std::string& user_id, const std::string& kind, Priority priority, nlohmann::json input,
                   const std::string& idempotency_key);

    // Фоновое задание импорта: мимо лимитов пользователя (busy,
    // too_many_active, rate_limited) — у импорта свои (один в работе на
    // пользователя, AI_IMPORTS_MAX_ACTIVE). Возвращает id.
    std::string create_internal(const std::string& user_id, const std::string& kind, nlohmann::json input);

    // Снимок задания. nullopt — нет, истекло или чужое (вызывающий отвечает
    // 404, не 403: не выдавать, что такой id есть). wait > 0 — long-poll:
    // вернуть, как только задание завершится, но не позже wait.
    std::optional<Job> get(const std::string& id, const std::string& user_id,
                           std::chrono::milliseconds wait = std::chrono::milliseconds(0));

    // Queued — снимается из очереди; Running — результат будет выброшен.
    // false — нет задания (или чужое), или оно уже завершено.
    bool cancel(const std::string& id, const std::string& user_id);

    // Воркер: следующее задание (интерактивные раньше фоновых), переводит его
    // в Running. Блокирует, пока заданий нет; nullopt — после stop().
    std::optional<Job> take();

    void complete(const std::string& id, Status status, nlohmann::json result, const std::string& failure);

    // Удаляет завершённые задания старше TTL. Зовётся фоновым потоком.
    void sweep();

    void stop();

    std::size_t queued(Priority p) const;

private:
    bool user_over_rate(const std::string& user_id, Clock::time_point now);
    std::size_t active_of(const std::string& user_id) const;

    Limits limits_;
    NowFn now_;
    mutable std::mutex mu_;
    std::condition_variable work_cv_;   // появилось задание / stop
    std::condition_variable change_cv_; // задание завершилось — для long-poll
    bool stopped_ = false;
    std::unordered_map<std::string, Job> jobs_;
    std::deque<std::string> interactive_;
    std::deque<std::string> background_;
    std::map<std::pair<std::string, std::string>, std::string> by_key_; // (user, key) -> id
    std::unordered_map<std::string, std::deque<Clock::time_point>> created_by_user_;
};

} // namespace jobs
