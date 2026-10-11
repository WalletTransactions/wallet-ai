#include "ai.hpp"

#include "session.hpp"
#include "util/log.hpp"

using nlohmann::json;

namespace aiapi {

namespace {

void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

void handle_status(AppState& state, const httplib::Request& req, httplib::Response& res) {
    applog::set_route("GET /api/ai/status");
    if (!session::current_user(req, *state.jwks)) {
        reply(res, 401, {{"error", "требуется вход: Authorization: Bearer или cookie session"}});
        return;
    }
    reply(res, 200, status_json(state.llm, state.llm_status.load()));
}

} // namespace

json status_json(const llm::Config& cfg, llm::ProbeResult status) {
    bool enabled = status == llm::ProbeResult::Ok;
    json j;
    j["enabled"] = enabled;
    // Имя модели — только когда ИИ включён: при выключенном оно ничего не
    // говорит клиенту, а детали поломки — в логах и метрике llm_up.
    j["model"] = enabled ? json(cfg.model) : json(nullptr);
    j["reason"] = enabled ? json(nullptr) : json(llm::to_string(status));
    j["features"] = json::array();
    return j;
}

void register_routes(httplib::Server& svr, AppState& state) {
    svr.Get("/api/ai/status",
            [&state](const httplib::Request& req, httplib::Response& res) { handle_status(state, req, res); });
}

} // namespace aiapi
