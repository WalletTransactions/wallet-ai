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

#include "access.hpp"
#include "ai.hpp"
#include "app_state.hpp"
#include "docs.hpp"
#include "httplib.h"
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

} // namespace

int main() {
    dotenv::load(".env");

    std::string log_dir = env_or("LOG_DIR", "");
    if (log_dir == "stdout" || log_dir == "-") log_dir.clear();
    applog::init(log_dir, "wallet-ai");
    metrics::set_build_info(version::kService, version::kVersion, version::kGitCommit);

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
    aiapi::register_routes(svr, state);
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

    applog::info("wallet-ai v" + std::string(version::kVersion) + " (" + std::string(version::kGitCommit) +
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
