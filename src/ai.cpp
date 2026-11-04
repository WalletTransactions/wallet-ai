#include "ai.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

#include "chat.hpp"
#include "parse_expense.hpp"
#include "session.hpp"
#include "util/log.hpp"
#include "util/uuid.hpp"
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
    metrics::set_gauge("ai_queue_cloud", "Заданий в очереди облачной полосы.",
                       static_cast<double>(store.queued(jobs::Priority::Interactive, jobs::Lane::Cloud)));
}

void count_create(const std::string& result) {
    metrics::inc_counter("ai_job_create_total", "Создание заданий по исходу.", "result", result);
}

void handle_status(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/ai/status");
    if (!session::current_user(req, *state.jwks)) return reply_unauthenticated(res);
    reply(res, 200, status_json(state.llm, state.llm_status.load(), state.imports_enabled && state.db_up.load(),
                                state.chat_enabled));
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
    if (kind != parse_expense::kKind && kind != chat::kKind) {
        return reply(res, 400, {{"error", "неизвестный kind: " + kind}});
    }
    json input = body.value("input", json());
    std::string key = body.contains("idempotency_key") && body["idempotency_key"].is_string()
                          ? body["idempotency_key"].get<std::string>()
                          : std::string{};
    if (kind == chat::kKind) {
        providers::Reply r = create_chat_job(state, *user, input, key);
        return reply(res, r.status, r.body);
    }
    if (std::string err = parse_expense::validate_input(input); !err.empty()) return reply(res, 400, {{"error", err}});

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

void handle_settings_get(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/ai/settings");
    auto user = session::current_user(req, *state.jwks);
    if (!user) return reply_unauthenticated(res);
    providers::SettingsRepo* repo = settings_repo(state);
    providers::Reply r;
    try {
        r = providers::get_settings(*state.providers, repo, user->user_id, state.llm_status.load());
    } catch (const std::exception& e) {
        // База пропала посреди запроса — выбор по умолчанию, как без базы.
        applog::warn("ai settings read failed", {{"event", "settings_error"}, {"error", e.what()}});
        r = providers::get_settings(*state.providers, nullptr, user->user_id, state.llm_status.load());
    }
    reply(res, r.status, r.body);
}

void handle_settings_put(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("PUT /api/ai/settings");
    auto user = session::current_user(req, *state.jwks);
    if (!user) return reply_unauthenticated(res);
    json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    providers::Reply r;
    try {
        r = providers::put_settings(*state.providers, settings_repo(state), user->user_id, body,
                                    state.llm_status.load(), static_cast<providers::Epoch>(std::time(nullptr)));
    } catch (const std::exception& e) {
        applog::warn("ai settings write failed", {{"event", "settings_error"}, {"error", e.what()}});
        r = {503, {{"error", "settings_unavailable"}}};
    }
    if (r.status == 200) {
        applog::info("ai settings changed", {{"event", "settings_changed"},
                                              {"provider", r.body.value("provider", std::string{})}});
    }
    reply(res, r.status, r.body);
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

json status_json(const llm::Config& cfg, llm::ProbeResult status, bool statement_import, bool chat) {
    bool enabled = status == llm::ProbeResult::Ok;
    json j;
    j["enabled"] = enabled;
    // Имя модели — только когда ИИ включён: при выключенном оно ничего не
    // говорит клиенту, а детали поломки — в логах и метрике llm_up.
    j["model"] = enabled ? json(cfg.model) : json(nullptr);
    j["reason"] = enabled ? json(nullptr) : json(llm::to_string(status));
    j["features"] = enabled ? json::array({parse_expense::kKind}) : json::array();
    if (enabled && statement_import) j["features"].push_back("statement_import");
    if (enabled && chat) j["features"].push_back(chat::kKind);
    return j;
}

providers::SettingsRepo* settings_repo(AppState& state) {
    return state.settings_repo && state.db_up.load() ? state.settings_repo.get() : nullptr;
}

providers::Reply create_chat_job(AppState& state, const session::User& user, const json& input,
                                 const std::string& idempotency_key) {
    if (std::string err = chat::validate_input(input); !err.empty()) return {400, {{"error", err}}};
    // Без wallet-mcp или секрета обмена чат выключен — google-auth не зовём.
    if (!state.chat_enabled || !state.providers || !state.auth_exchange) {
        count_create("unavailable");
        return {503, {{"error", "unavailable"}}};
    }

    // Провайдер — выбранный пользователем на момент создания (без подмены).
    providers::UserSettings settings;
    if (providers::SettingsRepo* repo = settings_repo(state)) {
        try {
            settings = repo->get(user.user_id);
        } catch (const std::exception& e) {
            applog::warn("ai settings read failed", {{"event", "settings_error"}, {"error", e.what()}});
        }
    }
    std::string provider = providers::effective_provider(*state.providers, settings);
    const providers::ProviderInfo* info = state.providers->find(provider);
    if (info == nullptr || !state.providers->availability(user.user_id, provider, state.llm_status.load()).available ||
        state.providers->model(provider) == nullptr) {
        count_create("provider_unavailable");
        return {503, {{"error", "provider_unavailable"}, {"provider", provider}}};
    }

    // Токен для wallet-mcp — сейчас, пока токен пользователя свежий (D4).
    auth_exchange::Result ex = state.auth_exchange->exchange(user.token);
    if (ex.status == auth_exchange::Status::Invalid) {
        applog::warn("token exchange refused", {{"event", "token_exchange_failed"}, {"error", ex.error}});
        count_create("invalid_token");
        return {401, {{"error", "invalid_token"}}};
    }
    if (ex.status != auth_exchange::Status::Ok) {
        json fields = {{"event", "token_exchange_failed"}, {"error", ex.error}};
        if (ex.invalid_client) {
            applog::error("google-auth rejected AUTH_EXCHANGE_SECRET (invalid_client)", fields);
        } else {
            applog::warn("token exchange unavailable", fields);
        }
        count_create("unavailable");
        return {503, {{"error", "unavailable"}}};
    }

    std::string conversation_id = input.contains("conversation_id") && input["conversation_id"].is_string()
                                      ? input["conversation_id"].get<std::string>()
                                      : uuidgen::v4();
    jobs::Lane lane = info->cloud ? jobs::Lane::Cloud : jobs::Lane::Local;
    jobs::Created created = state.jobs->create(user.user_id, chat::kKind, jobs::Priority::Interactive,
                                               chat::make_job_input(input, conversation_id, provider, ex.access_token),
                                               idempotency_key, lane);
    if (created.error != jobs::CreateError::None) {
        count_create(jobs::to_string(created.error));
        return {429, {{"error", jobs::to_string(created.error)}}};
    }
    count_create(created.reused ? "reused" : "created");
    metrics::inc_counter("ai_jobs_by_platform_total", "Созданные задания по платформе токена.", "platform",
                         user.platform.empty() ? "web" : user.platform);
    update_queue_gauges(*state.jobs);
    applog::info("job created", {{"event", "job_created"},
                                 {"job_id", created.job.id},
                                 {"kind", chat::kKind},
                                 {"provider", provider},
                                 {"lane", jobs::to_string(lane)},
                                 {"reused", created.reused},
                                 {"platform", user.platform}});
    return {202, {{"job_id", created.job.id}, {"status", jobs::to_string(created.job.status)}}};
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
    if (job.kind == chat::kKind && state.chat_agent) return chat::run(job, *state.chat_agent);
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
    svr.Get("/api/ai/settings",
            [&state](const httplib::Request& req, httplib::Response& res) { handle_settings_get(state, req, res); });
    svr.Put("/api/ai/settings",
            [&state](const httplib::Request& req, httplib::Response& res) { handle_settings_put(state, req, res); });
    svr.Get(R"(/api/ai/jobs/([0-9a-fA-F-]{36}))",
            [&state](const httplib::Request& req, httplib::Response& res) { handle_get(state, req, res); });
    svr.Delete(R"(/api/ai/jobs/([0-9a-fA-F-]{36}))",
               [&state](const httplib::Request& req, httplib::Response& res) { handle_cancel(state, req, res); });
}

void run_worker(AppState& state, jobs::Lane lane) {
    for (;;) {
        auto job = state.jobs->take(lane);
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
        details["lane"] = jobs::to_string(job->lane);
        details["result"] = result;
        details["queue_ms"] = static_cast<long>(queue_secs * 1000);
        details["run_ms"] = static_cast<long>(run_secs * 1000);
        applog::set_user(job->user_id);
        applog::info("job finished", details);
        applog::clear_request_context();
    }
}

} // namespace aiapi
