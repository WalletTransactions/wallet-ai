// wallet-ai — единая точка доступа к LLM (org/docs/TODO_wallet_ai.md):
// разбор трат текстом, импорт выписок, чат с инструментами. Клиенты — веб
// (через гейтвей, /api/ai/*) и Telegram-бот (напрямую).
//
// Инвариант: сервис ничего не пишет в кошелёк. Он возвращает черновики и
// читает данные токеном пользователя; запись делает клиент после
// подтверждения. Токены не выпускает — только проверяет по JWKS google-auth.
//
// Каркас, проверка токена (aud wallet-ai, scope ai:use), GET /api/ai/status,
// фоновая проба LLM; задания (/api/ai/jobs) и их воркеры — шаг 3.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <ctime>

#include "access.hpp"
#include "ai.hpp"
#include "app_state.hpp"
#include "docs.hpp"
#include "httplib.h"
#include "imports_http.hpp"
#include "statement/repo.hpp"
#include "llm_probe.hpp"
#include "nlohmann/json.hpp"
#include "util/dotenv.hpp"
#include "util/jwks.hpp"
#include "util/log.hpp"
#include "util/metrics.hpp"
#include "version.hpp"

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

std::string env_required(const char* key, const std::string& hint) {
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') {
        applog::error(std::string(key) + " must be set — " + hint + " (см. README.md)");
        std::exit(1);
    }
    return std::string(v);
}

std::string strip_trailing_slash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

// Тела запросов в лог не пишем вовсе: здесь будут тексты трат и выписок
// (TODO_wallet_ai.md, §9; приватность — §4.3 плана бота).

// trace_id ставит гейтвей (или бот) — сам сервис новый не порождает.
httplib::Server::HandlerResponse handle_pre_routing(const httplib::Request& req, httplib::Response& res) {
    applog::start_request_timer();
    std::string trace_id = req.get_header_value("X-Trace-Id");
    if (!trace_id.empty()) {
        applog::set_trace_id(trace_id);
        res.set_header("X-Trace-Id", trace_id);
    }
    return httplib::Server::HandlerResponse::Unhandled;
}

void set_llm_status(AppState& state, llm::ProbeResult r) {
    llm::ProbeResult prev = state.llm_status.exchange(r);
    metrics::set_gauge("llm_up", "LLM отвечает и нужная модель загружена (1/0).", r == llm::ProbeResult::Ok ? 1 : 0);
    // В лог — только смена состояния, а не каждая проба.
    if (prev == r) return;
    nlohmann::json details = {{"event", "llm_probe"}, {"result", llm::to_string(r)}};
    if (r == llm::ProbeResult::Ok) {
        applog::info("LLM available", details);
    } else if (r == llm::ProbeResult::Unauthorized) {
        applog::error("LLM answered 401/403 — check LLM_API_KEY", details);
    } else if (r != llm::ProbeResult::Disabled) {
        applog::warn("LLM unavailable", details);
    }
}

int env_int(const char* key, int fallback) { return std::stoi(env_or(key, std::to_string(fallback))); }

// Импорт выписок (openspec/changes/add-statement-import). Необязателен: без
// DATABASE_URL или промптов сервис работает, а /api/ai/imports отвечает 503.
// База может подняться позже сервиса — подключение повторяется в фоне;
// указатели в state выставляются до db_up = true, хендлеры смотрят на db_up.
void setup_imports(AppState& state, const std::string& prompt_dir, const std::string& database_url) {
    if (database_url.empty()) {
        applog::warn("DATABASE_URL не задан — импорт выписок выключен", {{"event", "imports_disabled"}});
        return;
    }
    llm::StatementPrompts prompts;
    try {
        prompts.rows = llm::load_prompt(prompt_dir + "/extract_rows.txt");
        prompts.classify = llm::load_prompt(prompt_dir + "/classify_rows.txt");
    } catch (const std::exception& e) {
        applog::error(std::string("импорт выписок выключен: ") + e.what(), {{"event", "imports_disabled"}});
        return;
    }
    // Клиент LLM с промптами импорта заменяет созданный выше (тот же конфиг).
    state.llm_client = llm::make_http_client(state.llm, state.expense_prompt, prompts);
    state.imports_enabled = true;

    imports::Config cfg;
    cfg.chunk_chars = static_cast<std::size_t>(env_int("AI_EXTRACT_CHUNK_CHARS", 1800));
    cfg.llm_context = env_int("LLM_CONTEXT", 8192);
    cfg.max_active = env_int("AI_IMPORTS_MAX_ACTIVE", 3);
    cfg.ttl_days = env_int("AI_IMPORT_TTL_DAYS", 90);
    cfg.done_ttl_days = env_int("AI_IMPORT_DONE_TTL_DAYS", 7);
    cfg.pause_max_mins = env_int("AI_IMPORT_PAUSE_MAX_MINS", 60);
    cfg.classify_min_confidence = std::stod(env_or("AI_CLASSIFY_MIN_CONFIDENCE", "0.7"));
    cfg.profile_max_tokens = static_cast<std::size_t>(env_int("AI_PROFILE_MAX_TOKENS", 500));
    auto profiles = stmt::load_profiles(prompt_dir + "/banks");

    llm::CallOptions extract_opts; // max_tokens — по числу строк фрагмента
    extract_opts.reasoning_effort = "none";
    extract_opts.timeout_ms = env_int("AI_JOB_TIMEOUT_SECS_EXTRACT_ROWS", 180) * 1000;
    llm::CallOptions classify_opts;
    classify_opts.reasoning_effort = "none";
    classify_opts.max_tokens = env_int("LLM_MAX_TOKENS_CLASSIFY_ROWS", 1024);
    classify_opts.timeout_ms = env_int("AI_JOB_TIMEOUT_SECS_CLASSIFY_ROWS", 60) * 1000;

    // Обработчики видов — сразу: воркеры стартуют раньше, чем база.
    state.job_handlers["extract_rows"] = [&state](const jobs::Job& job) {
        return state.db_up.load() ? state.import_runner->handle_extract(job)
                                  : parse_expense::Outcome{jobs::Status::Failed, nullptr, "unavailable", {}};
    };
    state.job_handlers["classify_rows"] = [&state](const jobs::Job& job) {
        return state.db_up.load() ? state.import_runner->handle_classify(job)
                                  : parse_expense::Outcome{jobs::Status::Failed, nullptr, "unavailable", {}};
    };

    std::size_t pool_size = static_cast<std::size_t>(std::max(1, env_int("AI_DB_POOL_SIZE", 4)));
    std::thread([&state, database_url, pool_size, cfg, profiles = std::move(profiles), extract_opts,
                 classify_opts]() mutable {
        // Подключение и проверка схемы — до успеха, раз в 30 с.
        for (;;) {
            try {
                auto pool = std::make_unique<db::Pool>(database_url, pool_size);
                pool->acquire()->verify_schema();
                state.db = std::move(pool);
                break;
            } catch (const std::exception& e) {
                applog::warn("база ai недоступна, импорт выписок ждёт", {{"event", "db_unavailable"},
                                                                         {"error", e.what()}});
                std::this_thread::sleep_for(std::chrono::seconds(30));
            }
        }
        state.repo = stmt::make_pg_repo(*state.db);
        state.import_service = std::make_unique<imports::Service>(
            *state.repo, *state.reports, std::move(profiles), cfg,
            [] { return static_cast<stmt::Epoch>(std::time(nullptr)); });
        state.import_runner = std::make_unique<imports::Runner>(
            *state.repo, *state.jobs, *state.llm_client, *state.import_service,
            [&state] { return state.llm_status.load() == llm::ProbeResult::Ok; }, extract_opts, classify_opts);
        state.import_service->on_change = [&state] { state.import_runner->wake(); };
        state.import_service->on_delete = [&state](const std::string& id) { state.import_runner->cancel_import(id); };
        state.db_up.store(true);
        applog::info("база ai подключена, импорт выписок включён", {{"event", "db_connected"}});

        std::thread([&state] { state.import_runner->run([&state] { return state.db_up.load(); }); }).detach();
        // Проба базы: статус и 503 видят, что Postgres пропал.
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            bool up = state.repo->ping();
            if (up != state.db_up.exchange(up)) {
                applog::warn(up ? "база ai снова доступна" : "база ai недоступна", {{"event", "db_probe"},
                                                                                     {"up", up}});
            }
        }
    }).detach();
}

} // namespace

int main() {
    dotenv::load(".env");

    std::string log_dir = env_or("LOG_DIR", "");
    if (log_dir == "stdout" || log_dir == "-") log_dir.clear();
    applog::init(log_dir, "wallet-ai");
    metrics::set_build_info(version::kService, version::kVersion, version::kBuild,
                            version::kGitCommit);

    AppState state;

    std::string auth_service_url = strip_trailing_slash(
        env_required("AUTH_SERVICE_URL", "адрес auth-сервиса, например http://127.0.0.1:8080"));
    state.jwks = std::make_unique<jwks::Cache>(auth_service_url);

    state.llm.url = strip_trailing_slash(env_or("LLM_URL", ""));
    state.llm.model = env_or("LLM_MODEL", "");
    state.llm.api_key = env_or("LLM_API_KEY", "");
    state.llm.timeout_ms = std::stoi(env_or("LLM_TIMEOUT_MS", "15000"));
    if (!state.llm.url.empty() && state.llm.model.empty()) {
        applog::error("LLM_MODEL must be set when LLM_URL is set — id из GET {LLM_URL}/models (см. README.md)");
        return 1;
    }
    if (!state.llm.url.empty() && llm::split_base_url(state.llm.url).origin.empty()) {
        applog::error("LLM_URL должен быть вида http://host:port/v1, получено: " + state.llm.url);
        return 1;
    }
    // Промпты — файлы рядом с исполняемым файлом (как openapi.yaml): правка
    // без пересборки, нужен рестарт. Нет файла — не стартуем: без промпта
    // сервис бесполезен, а молча работать на чём-то ещё хуже.
    std::string prompt_dir = strip_trailing_slash(env_or("LLM_PROMPT_DIR", "prompts"));
    try {
        state.expense_prompt = llm::load_prompt(prompt_dir + "/expense.txt");
    } catch (const std::exception& e) {
        applog::error(e.what());
        return 1;
    }
    int probe_interval_secs = std::stoi(env_or("LLM_PROBE_INTERVAL_SECS", "60"));
    if (probe_interval_secs < 1) probe_interval_secs = 60;

    state.reports = reports::make_http_api(
        strip_trailing_slash(env_required("WALLET_REPORT_URL", "адрес wallet-report, например http://127.0.0.1:8084")),
        "ai_reports_errors_total");
    state.llm_client = llm::make_http_client(state.llm, state.expense_prompt);

    // Вид задания parse_expense (TODO_wallet_ai.md, §3, §5): сколько думать,
    // запас токенов под рассуждения, дедлайн генерации.
    state.parse_expense_opts.reasoning_effort = env_or("LLM_REASONING_PARSE_EXPENSE", "none");
    if (state.parse_expense_opts.reasoning_effort == "default") state.parse_expense_opts.reasoning_effort.clear();
    state.parse_expense_opts.max_tokens = std::stoi(env_or("LLM_MAX_TOKENS_PARSE_EXPENSE", "512"));
    state.parse_expense_opts.timeout_ms = std::stoi(env_or("AI_JOB_TIMEOUT_SECS_PARSE_EXPENSE", "60")) * 1000;

    jobs::Limits limits;
    limits.max_queued_interactive = static_cast<std::size_t>(std::stoi(env_or("AI_QUEUE_MAX", "20")));
    limits.max_active_per_user = static_cast<std::size_t>(std::stoi(env_or("AI_MAX_ACTIVE_PER_USER", "3")));
    limits.max_per_minute = std::stoi(env_or("LLM_MAX_PER_MINUTE", "10"));
    limits.ttl = std::chrono::seconds(std::stoi(env_or("AI_JOB_TTL_SECS", "600")));
    state.jobs = std::make_unique<jobs::Store>(limits);

    setup_imports(state, prompt_dir, env_or("DATABASE_URL", ""));

    // Воркеры = одновременные запросы к GPU (§4): на одной видеокарте — 1.
    int workers = std::max(1, std::stoi(env_or("LLM_MAX_CONCURRENT", "1")));
    for (int i = 0; i < workers; ++i) std::thread([&state] { aiapi::run_worker(state); }).detach();
    // Завершённые задания живут AI_JOB_TTL_SECS — потом удаляются.
    std::thread([&state] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            state.jobs->sweep();
        }
    }).detach();

    // Проба LLM: сразу при старте (LLM необязательна — недоступна, значит
    // status.enabled=false, сервис всё равно стартует) и дальше в фоне.
    auto probe = llm::make_http_probe(state.llm);
    set_llm_status(state, probe->probe());
    std::thread([&state, probe = std::move(probe), probe_interval_secs]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(probe_interval_secs));
            set_llm_status(state, probe->probe());
        }
    }).detach();

    std::string host = env_or("HOST", "127.0.0.1");
    int port = std::stoi(env_or("PORT", "8087"));

    httplib::Server svr;
    svr.set_pre_routing_handler([&state](const httplib::Request& req, httplib::Response& res) {
        handle_pre_routing(req, res);
        return authz::gate(req, res, *state.jwks);
    });
    // Выписка до 5 МБ в base64 (D9) — с запасом на JSON вокруг.
    svr.set_payload_max_length(8u * 1024 * 1024);
    aiapi::register_routes(svr, state);
    imports::register_routes(svr, state);
    docsapi::register_routes(svr); // GET /openapi.yaml, /docs, /healthz, /metrics, /version

    svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
        std::string message = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            message = e.what();
        }
        applog::error("unhandled exception: " + req.method + " " + req.path + " -> " + message);
        res.status = 500;
        res.set_content(nlohmann::json{{"error", "internal server error"}}.dump(), "application/json");
    });

    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        metrics::observe_http_request(req.method,
                                      applog::current_route().empty() ? "(unset)" : applog::current_route(),
                                      res.status, applog::request_duration_ms());
        if (req.path == "/healthz" || req.path == "/metrics") {
            applog::clear_request_context();
            return;
        }
        applog::info(req.method + " " + req.path + " -> " + std::to_string(res.status),
                     {{"event", "request_finished"}, {"duration_ms", applog::request_duration_ms()}});
        applog::clear_request_context();
    });

    applog::info("wallet-ai v" + std::string(version::kBuild) + " (" + std::string(version::kGitCommit) +
                 ") listening on http://" + host + ":" + std::to_string(port) + " (auth-сервис: " + auth_service_url +
                 ", LLM: " + (state.llm.url.empty() ? "выключена" : state.llm.url + " / " + state.llm.model) +
                 ", промпт: " + state.expense_prompt.version + ")");
    if (!svr.listen(host, port)) {
        applog::error("не удалось запустить сервер на " + host + ":" + std::to_string(port) +
                      " (порт занят или недоступен?)");
        return 1;
    }
    return 0;
}
