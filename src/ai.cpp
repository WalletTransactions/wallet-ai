#include "ai.hpp"

#include <algorithm>
#include <chrono>

#include "parse_expense.hpp"
#include "session.hpp"
#include "util/log.hpp"
#include "util/metrics.hpp"

using nlohmann::json;

namespace aiapi {

namespace {

void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

void reply_unauthenticated(httplib::Response& res) {
    reply(res, 401, {{"error", "требуется вход: Authorization: Bearer или cookie session"}});
}

long ms_between(jobs::Clock::time_point a, jobs::Clock::time_point b) {
    if (a == jobs::Clock::time_point{} || b == jobs::Clock::time_point{}) return 0;
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
}

void update_queue_gauges(const jobs::Store& store) {
    metrics::set_gauge("ai_queue_interactive", "Интерактивных заданий в очереди.",
                       static_cast<double>(store.queued(jobs::Priority::Interactive)));
    metrics::set_gauge("ai_queue_background", "Фоновых заданий в очереди.",
                       static_cast<double>(store.queued(jobs::Priority::Background)));
}

void handle_status(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/ai/status");
    if (!session::current_user(req, *state.jwks)) return reply_unauthenticated(res);
    reply(res, 200, status_json(state.llm, state.llm_status.load(), state.imports_enabled && state.db_up.load()));
}

void handle_create(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("POST /api/ai/jobs");
    auto user = session::current_user(req, *state.jwks);
    if (!user) return reply_unauthenticated(res);

    json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (!body.is_object()) return reply(res, 400, {{"error", "тело — JSON-объект {kind, input, idempotency_key?}"}});
    std::string kind = body.value("kind", std::string{});
    // Виды импорта выписки создаёт только сам сервис (спека ai-jobs).
    if (kind == "extract_rows" || kind == "classify_rows") return reply(res, 400, {{"error", "unknown_kind"}});
    if (kind != parse_expense::kKind) return reply(res, 400, {{"error", "неизвестный kind: " + kind}});
    json input = body.value("input", json());
    if (std::string err = parse_expense::validate_input(input); !err.empty()) return reply(res, 400, {{"error", err}});
    std::string key = body.contains("idempotency_key") && body["idempotency_key"].is_string()
                          ? body["idempotency_key"].get<std::string>()
                          : std::string{};

    // ИИ выключен — сразу, без задания: клиент уходит на строгий формат.
    if (state.llm_status.load() != llm::ProbeResult::Ok) {
        metrics::inc_counter("ai_job_create_total", "Создание заданий по исходу.", "result", "unavailable");
        return reply(res, 503, {{"error", "unavailable"}});
    }

    // Справочник — токеном пользователя, сейчас (к запуску токен мог истечь).
    auto accounts = state.reports->get("/api/reports/accounts", user->token);
    if (accounts.status == 401 || accounts.status == 403) {
        return reply(res, 401, {{"error", "wallet-report не принял токен"}});
    }
    if (accounts.status != 200) {
        applog::warn("wallet-report accounts failed", {{"event", "reports_error"}, {"error", accounts.error}});
        return reply(res, 502, {{"error", "wallet-report недоступен"}});
    }

    jobs::Created created = state.jobs->create(user->user_id, kind, jobs::Priority::Interactive,
                                               parse_expense::make_job_input(input, accounts.body), key);
    if (created.error != jobs::CreateError::None) {
        metrics::inc_counter("ai_job_create_total", "Создание заданий по исходу.", "result",
                             jobs::to_string(created.error));
        return reply(res, 429, {{"error", jobs::to_string(created.error)}});
    }
    metrics::inc_counter("ai_job_create_total", "Создание заданий по исходу.", "result",
                         created.reused ? "reused" : "created");
    metrics::inc_counter("ai_jobs_by_platform_total", "Созданные задания по платформе токена.", "platform",
                         user->platform.empty() ? "web" : user->platform);
    update_queue_gauges(*state.jobs);
    applog::info("job created", {{"event", "job_created"},
                                 {"job_id", created.job.id},
                                 {"kind", kind},
                                 {"reused", created.reused},
                                 {"platform", user->platform}});
    reply(res, 202, {{"job_id", created.job.id}, {"status", jobs::to_string(created.job.status)}});
}

void handle_get(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/ai/jobs/:id");
    auto user = session::current_user(req, *state.jwks);
    if (!user) return reply_unauthenticated(res);
    int wait = 0;
    if (req.has_param("wait")) {
        try {
            wait = std::clamp(std::stoi(req.get_param_value("wait")), 0, kMaxWaitSecs);
        } catch (const std::exception&) {
            return reply(res, 400, {{"error", "wait — секунды, 0.." + std::to_string(kMaxWaitSecs)}});
        }
    }
    auto job = state.jobs->get(req.matches[1], user->user_id, std::chrono::seconds(wait));
    if (!job) return reply(res, 404, {{"error", "задания нет или оно истекло"}});
    reply(res, 200, job_json(*job));
}

void handle_cancel(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("DELETE /api/ai/jobs/:id");
    auto user = session::current_user(req, *state.jwks);
    if (!user) return reply_unauthenticated(res);
    if (!state.jobs->cancel(req.matches[1], user->user_id)) {
        return reply(res, 404, {{"error", "задания нет, оно истекло или уже завершено"}});
    }
    update_queue_gauges(*state.jobs);
    res.status = 204;
}

} // namespace

json status_json(const llm::Config& cfg, llm::ProbeResult status, bool statement_import) {
    bool enabled = status == llm::ProbeResult::Ok;
    json j;
    j["enabled"] = enabled;
    // Имя модели — только когда ИИ включён: при выключенном оно ничего не
    // говорит клиенту, а детали поломки — в логах и метрике llm_up.
    j["model"] = enabled ? json(cfg.model) : json(nullptr);
    j["reason"] = enabled ? json(nullptr) : json(llm::to_string(status));
    j["features"] = enabled ? json::array({parse_expense::kKind}) : json::array();
    if (enabled && statement_import) j["features"].push_back("statement_import");
    return j;
}

json job_json(const jobs::Job& job) {
    json j = {{"job_id", job.id},
              {"kind", job.kind},
              {"status", jobs::to_string(job.status)},
              {"queue_ms", ms_between(job.created, job.started)},
              {"run_ms", ms_between(job.started, job.finished)}};
    if (job.status == jobs::Status::Done) j["result"] = job.result;
    if (job.status == jobs::Status::Failed) j["failure"] = job.failure;
    return j;
}

parse_expense::Outcome run_job(AppState& state, const jobs::Job& job) {
    if (job.kind == parse_expense::kKind) {
        return parse_expense::run(job.input, *state.llm_client, state.parse_expense_opts);
    }
    auto handler = state.job_handlers.find(job.kind);
    if (handler != state.job_handlers.end()) return handler->second(job);
    // Неизвестный вид — без вызова LLM.
    parse_expense::Outcome out;
    out.status = jobs::Status::Failed;
    out.failure = "internal";
    out.log = {{"error", "unknown_kind"}};
    return out;
}

void register_routes(httplib::Server& svr, AppState& state) {
    svr.Get("/api/ai/status",
            [&state](const httplib::Request& req, httplib::Response& res) { handle_status(state, req, res); });
    svr.Post("/api/ai/jobs",
             [&state](const httplib::Request& req, httplib::Response& res) { handle_create(state, req, res); });
    svr.Get(R"(/api/ai/jobs/([0-9a-fA-F-]{36}))",
            [&state](const httplib::Request& req, httplib::Response& res) { handle_get(state, req, res); });
    svr.Delete(R"(/api/ai/jobs/([0-9a-fA-F-]{36}))",
               [&state](const httplib::Request& req, httplib::Response& res) { handle_cancel(state, req, res); });
}

void run_worker(AppState& state) {
    for (;;) {
        auto job = state.jobs->take();
        if (!job) return;
        update_queue_gauges(*state.jobs);
        double queue_secs = ms_between(job->created, job->started) / 1000.0;
        metrics::observe_duration("ai_queue_wait_seconds", "Ожидание задания в очереди.", "kind", job->kind,
                                  queue_secs);

        auto started = std::chrono::steady_clock::now();
        parse_expense::Outcome out;
        try {
            out = run_job(state, *job);
        } catch (const std::exception& e) {
            out.status = jobs::Status::Failed;
            out.failure = "internal";
            out.log = {{"exception", e.what()}};
        }
        double run_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        state.jobs->complete(job->id, out.status, out.result, out.failure);

        std::string result = out.status == jobs::Status::Done ? out.log.value("outcome", std::string("done"))
                                                              : "failed_" + out.failure;
        metrics::inc_counter("ai_job_result_total", "Завершённые задания по исходу.", "result", result);
        metrics::observe_duration("ai_job_duration_seconds", "Выполнение задания (без очереди).", "kind", job->kind,
                                  run_secs);
        // Лог без текста и сумм (§5): исход, причины, токены, время.
        json details = out.log;
        details["event"] = "job_finished";
        details["job_id"] = job->id;
        details["kind"] = job->kind;
        details["result"] = result;
        details["queue_ms"] = static_cast<long>(queue_secs * 1000);
        details["run_ms"] = static_cast<long>(run_secs * 1000);
        applog::set_user(job->user_id);
        applog::info("job finished", details);
        applog::clear_request_context();
    }
}

} // namespace aiapi
