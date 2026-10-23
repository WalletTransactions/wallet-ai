// Импорт банковской выписки (openspec/changes/add-statement-import):
//   POST   /api/ai/imports                 — создать → 202 {import_id}
//   GET    /api/ai/imports                 — список, новые первыми
//   GET    /api/ai/imports/{id}            — статус, прогресс, строки и группы
//   DELETE /api/ai/imports/{id}            — отменить и удалить
//   POST   /api/ai/imports/{id}/retry      — продолжить failed
//   PATCH  /api/ai/imports/{id}/rows       — правки строк
//   POST   /api/ai/imports/{id}/rows       — строка из «не разобрано»
//   POST   /api/ai/imports/{id}/committed  — отметить записанное
//   GET/PATCH/DELETE /api/ai/statement-profiles[/{id}] — профили формата
//
// Service — логика ручек (тестируется без HTTP и Postgres, tests/mem_repo.hpp),
// Runner — планировщик фоновых заданий extract_rows/classify_rows (D5).
#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "clients/reports_client.hpp"
#include "jobs.hpp"
#include "llm_client.hpp"
#include "nlohmann/json.hpp"
#include "parse_expense.hpp"
#include "statement/profiles.hpp"
#include "statement/repo.hpp"

namespace imports {

struct Config {
    std::size_t chunk_chars = 1800;        // AI_EXTRACT_CHUNK_CHARS
    std::size_t min_chunk_chars = 600;     // если окно модели не вмещает настроенный
    int llm_context = 8192;                // LLM_CONTEXT — окно модели в LM Studio (scripts/lms-load.sh)
    std::size_t max_chars = 300000;
    int max_chunks = 80;
    std::size_t file_max_bytes = 5u * 1024 * 1024;
    int max_active = 3;                    // AI_IMPORTS_MAX_ACTIVE
    int ttl_days = 90;                     // AI_IMPORT_TTL_DAYS
    int done_ttl_days = 7;                 // AI_IMPORT_DONE_TTL_DAYS
    int pause_max_mins = 60;               // AI_IMPORT_PAUSE_MAX_MINS
    double classify_min_confidence = 0.7;  // AI_CLASSIFY_MIN_CONFIDENCE
    std::size_t profile_max_tokens = 500;  // AI_PROFILE_MAX_TOKENS
    int classify_batch = 20;
    int chunk_max_failures = 3;
    int reconcile_cache_secs = 60;
    int examples_per_format = 5;
};

using NowFn = std::function<stmt::Epoch()>;

struct Reply {
    int status = 200;
    nlohmann::json body;
};

struct User {
    std::string user_id;
    std::string token;
};

// Группа строки для проверки (statement-review, «Группы строк»).
std::string group_of(const stmt::RowRec& r);

// fingerprint и op_uuid для незаписанных строк: k — номер среди одинаковых
// (дата, сумма, направление, pattern) в порядке строк импорта.
void assign_fingerprints(std::vector<stmt::RowRec>& rows, const std::string& wallet_uuid);

// Операция кошелька из GET /api/reports/operations для сверки.
struct WalletOp {
    std::string uuid;
    std::string date; // ГГГГ-ММ-ДД
    long long cents = 0;
};

// Сверка с внесёнными (D7): imported по отпечаткам, затем exists /
// possible_duplicate по сумме и дате ±1 день, одна операция — одна строка.
void reconcile(std::vector<stmt::RowRec>& rows, const std::vector<WalletOp>& ops,
               const std::set<std::string>& imported_fingerprints);

class Service {
public:
    Service(stmt::Repo& repo, reports::ReportsApi& reports, std::vector<stmt::Profile> profiles, Config cfg,
            NowFn now);

    Reply create(const User& u, const nlohmann::json& body);
    Reply list(const User& u);
    Reply get(const User& u, const std::string& id, bool force_reconcile);
    Reply remove(const User& u, const std::string& id);
    Reply retry(const User& u, const std::string& id);
    Reply patch_rows(const User& u, const std::string& id, const nlohmann::json& body);
    Reply add_row(const User& u, const std::string& id, const nlohmann::json& body);
    Reply committed(const User& u, const std::string& id, const nlohmann::json& body);

    Reply profiles(const User& u);
    Reply patch_profile(const User& u, const std::string& id, const nlohmann::json& body);
    Reply delete_profile(const User& u, const std::string& id);

    // Для планировщика: часть промпта профиля и параметры проверки импорта.
    std::string profile_prompt_for(const stmt::ImportRec& imp);
    stmt::CheckRules check_rules_for(const stmt::ImportRec& imp) const;
    const std::vector<stmt::Profile>& builtin_profiles() const { return profiles_; }

    // Будит планировщик (новый импорт, retry) и снимает задания удалённого.
    std::function<void()> on_change;
    std::function<void(const std::string& import_id)> on_delete;

    const Config& config() const { return cfg_; }
    stmt::Epoch now() const { return now_(); }

private:
    nlohmann::json view(const stmt::ImportRec& imp, const std::vector<stmt::RowRec>* rows, bool reconciled);
    nlohmann::json profile_json(const stmt::ImportRec& imp);
    bool fetch_ops(const User& u, const stmt::ImportRec& imp, const std::vector<stmt::RowRec>& rows,
                   std::vector<WalletOp>& out);
    void maybe_done(stmt::ImportRec& imp, const std::vector<stmt::RowRec>& rows);
    void learn_format(const User& u, const stmt::ImportRec& imp, const stmt::RowRec& row);

    stmt::Repo& repo_;
    reports::ReportsApi& reports_;
    std::vector<stmt::Profile> profiles_;
    Config cfg_;
    NowFn now_;
};

// Планировщик (D5): не больше одного задания на импорт в фоновой очереди,
// импорты по кругу, пауза без LLM, возобновление после рестарта по
// statement_chunk.status. Обработчики заданий вызывает воркер (ai.cpp).
class Runner {
public:
    Runner(stmt::Repo& repo, jobs::Store& store, llm::Client& llm, Service& service, std::function<bool()> llm_ok,
           llm::CallOptions extract_opts, llm::CallOptions classify_opts);

    // Один проход планирования. Зовётся потоком run() и тестами.
    void tick();
    // Чистка по TTL (раз в час из run()).
    int sweep();

    parse_expense::Outcome handle_extract(const jobs::Job& job);
    parse_expense::Outcome handle_classify(const jobs::Job& job);

    void cancel_import(const std::string& import_id);
    void wake();
    // Цикл: tick по событию или раз в 5 с, sweep раз в час; db_up — база жива.
    void run(const std::function<bool()>& db_up);

private:
    void finish_extraction(stmt::ImportRec& imp);
    void finish_ready(stmt::ImportRec& imp);
    void fail(stmt::ImportRec& imp, const std::string& failure);
    void done_with(const std::string& import_id);

    stmt::Repo& repo_;
    jobs::Store& store_;
    llm::Client& llm_;
    Service& service_;
    std::function<bool()> llm_ok_;
    llm::CallOptions extract_opts_;
    llm::CallOptions classify_opts_;

    std::mutex mu_;
    std::condition_variable cv_;
    bool woken_ = false;
    // import_id → (job_id, user_id) задания в очереди или в работе.
    std::map<std::string, std::pair<std::string, std::string>> inflight_;
    stmt::Epoch last_sweep_ = 0;

    void step(stmt::ImportRec& imp, bool llm_ok, stmt::Epoch now);
    void enqueue(const stmt::ImportRec& imp, const std::string& kind, nlohmann::json input);
    parse_expense::Outcome extract(const jobs::Job& job);
    parse_expense::Outcome classify(const jobs::Job& job);
};

} // namespace imports
