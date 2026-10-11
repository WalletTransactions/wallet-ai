#include "session.hpp"

#include "util/jwt.hpp"
#include "util/log.hpp"

using nlohmann::json;

namespace session {

namespace {

constexpr const char* kSessionCookie = "session";

std::string extract_bearer(const httplib::Request& req) {
    std::string header = req.get_header_value("Authorization");
    static const std::string prefix = "Bearer ";
    if (header.size() > prefix.size() && header.compare(0, prefix.size(), prefix) == 0) {
        return header.substr(prefix.size());
    }
    return {};
}

// cpp-httplib не разбирает куки за нас.
std::string extract_cookie(const httplib::Request& req, const std::string& name) {
    std::string header = req.get_header_value("Cookie");
    size_t pos = 0;
    while (pos <= header.size()) {
        size_t semi = header.find(';', pos);
        std::string piece =
            header.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);

        size_t key_start = piece.find_first_not_of(' ');
        if (key_start != std::string::npos) {
            size_t eq = piece.find('=', key_start);
            if (eq != std::string::npos) {
                std::string key = piece.substr(key_start, eq - key_start);
                if (key == name) return piece.substr(eq + 1);
            }
        }

        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return {};
}

} // namespace

std::string extract_token(const httplib::Request& req) {
    std::string token = extract_bearer(req);
    if (token.empty()) token = extract_cookie(req, kSessionCookie);
    return token;
}

std::optional<json> verified_payload(const httplib::Request& req, jwks::Cache& jwks_cache) {
    std::string token = extract_token(req);
    if (token.empty()) return std::nullopt;

    auto kid = jwt::peek_kid(token);
    if (!kid) return std::nullopt;

    auto key = jwks_cache.find(*kid);
    if (!key) return std::nullopt;

    return jwt::verify_rs256(token, key->n, key->e);
}

std::optional<User> current_user(const httplib::Request& req, jwks::Cache& jwks_cache) {
    auto payload = verified_payload(req, jwks_cache);
    if (!payload) return std::nullopt;

    try {
        User user;
        user.user_id = payload->at("sub").get<std::string>();
        user.platform = payload->value("platform", std::string{});
        user.token = extract_token(req);
        applog::set_user(user.user_id);
        return user;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace session
