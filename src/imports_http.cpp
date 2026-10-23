// HTTP-обвязка импорта выписок: токен, JSON, 503 без базы — логика в imports.cpp.
#include "imports_http.hpp"

#include "session.hpp"
#include "util/log.hpp"

using nlohmann::json;

namespace imports {

namespace {

void reply(httplib::Response& res, const Reply& r) {
    res.status = r.status;
    if (!r.body.is_null()) res.set_content(r.body.dump(), "application/json; charset=utf-8");
}

using Handler = std::function<Reply(Service&, const User&, const httplib::Request&, const json&)>;

// Общая часть ручек: вход, импорт включён и база отвечает, тело — JSON.
void handle(AppState& state, const char* route, const httplib::Request& req, httplib::Response& res,
            const Handler& h) {
    applog::set_route(route);
    auto user = session::current_user(req, *state.jwks);
    if (!user) return reply(res, {401, {{"error", "требуется вход: Authorization: Bearer или cookie session"}}});
    if (!state.import_service || !state.db_up.load()) return reply(res, {503, {{"error", "unavailable"}}});
    json body;
    if (!req.body.empty()) {
        body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
        if (body.is_discarded()) return reply(res, {400, {{"error", "bad_request"}}});
    }
    try {
        reply(res, h(*state.import_service, {user->user_id, user->token}, req, body));
    } catch (const std::exception& e) {
        // Чаще всего — база пропала посреди запроса.
        applog::error("import request failed", {{"event", "import_request_failed"}, {"error", e.what()}});
        reply(res, {503, {{"error", "unavailable"}}});
    }
}

} // namespace

void register_routes(httplib::Server& svr, AppState& state) {
    const std::string id = "([0-9a-fA-F-]{36})";
    auto route = [&state](const char* name, Handler h) {
        return [&state, name, h](const httplib::Request& req, httplib::Response& res) {
            handle(state, name, req, res, h);
        };
    };
    svr.Post("/api/ai/imports", route("POST /api/ai/imports", [](Service& s, const User& u, const auto&, const json& b) {
                 return s.create(u, b);
             }));
    svr.Get("/api/ai/imports", route("GET /api/ai/imports", [](Service& s, const User& u, const auto&, const json&) {
                return s.list(u);
            }));
    svr.Get("/api/ai/imports/" + id,
            route("GET /api/ai/imports/:id", [](Service& s, const User& u, const httplib::Request& r, const json&) {
                return s.get(u, r.matches[1], r.get_param_value("reconcile") == "1");
            }));
    svr.Delete("/api/ai/imports/" + id,
               route("DELETE /api/ai/imports/:id", [](Service& s, const User& u, const httplib::Request& r,
                                                      const json&) { return s.remove(u, r.matches[1]); }));
    svr.Post("/api/ai/imports/" + id + "/retry",
             route("POST /api/ai/imports/:id/retry", [](Service& s, const User& u, const httplib::Request& r,
                                                        const json&) { return s.retry(u, r.matches[1]); }));
    svr.Patch("/api/ai/imports/" + id + "/rows",
              route("PATCH /api/ai/imports/:id/rows", [](Service& s, const User& u, const httplib::Request& r,
                                                         const json& b) { return s.patch_rows(u, r.matches[1], b); }));
    svr.Post("/api/ai/imports/" + id + "/rows",
             route("POST /api/ai/imports/:id/rows", [](Service& s, const User& u, const httplib::Request& r,
                                                       const json& b) { return s.add_row(u, r.matches[1], b); }));
    svr.Post("/api/ai/imports/" + id + "/committed",
             route("POST /api/ai/imports/:id/committed",
                   [](Service& s, const User& u, const httplib::Request& r, const json& b) {
                       return s.committed(u, r.matches[1], b);
                   }));
    svr.Get("/api/ai/statement-profiles",
            route("GET /api/ai/statement-profiles",
                  [](Service& s, const User& u, const auto&, const json&) { return s.profiles(u); }));
    svr.Patch("/api/ai/statement-profiles/([^/]+)",
              route("PATCH /api/ai/statement-profiles/:id",
                    [](Service& s, const User& u, const httplib::Request& r, const json& b) {
                        return s.patch_profile(u, r.matches[1], b);
                    }));
    svr.Delete("/api/ai/statement-profiles/([^/]+)",
               route("DELETE /api/ai/statement-profiles/:id",
                     [](Service& s, const User& u, const httplib::Request& r, const json&) {
                         return s.delete_profile(u, r.matches[1]);
                     }));
}

} // namespace imports
