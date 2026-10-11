#include "util/jwt_claims.hpp"

#include <sstream>

namespace jwt_claims {

bool has_scope(const nlohmann::json& payload, const std::string& scope) {
    auto it = payload.find("scope");
    if (it == payload.end() || !it->is_string()) return false;
    std::istringstream ss(it->get<std::string>());
    std::string item;
    while (ss >> item) {
        if (item == scope) return true;
    }
    return false;
}

Result check(const nlohmann::json& payload, const std::string& audience, const std::string& required_scope) {
    auto aud = payload.find("aud");
    if (aud == payload.end()) return Result::Legacy;

    auto iss = payload.find("iss");
    if (iss == payload.end() || !iss->is_string() || iss->get<std::string>() != kIssuer) {
        return Result::WrongIssuer;
    }

    // aud — строка или массив строк (RFC 7519, 4.1.3).
    bool aud_ok = false;
    if (aud->is_string()) {
        aud_ok = aud->get<std::string>() == audience;
    } else if (aud->is_array()) {
        for (const auto& a : *aud) {
            if (a.is_string() && a.get<std::string>() == audience) aud_ok = true;
        }
    }
    if (!aud_ok) return Result::WrongAudience;

    if (!required_scope.empty() && !has_scope(payload, required_scope)) return Result::InsufficientScope;
    return Result::Ok;
}

const char* to_string(Result r) {
    switch (r) {
    case Result::Ok: return "ok";
    case Result::Legacy: return "legacy";
    case Result::WrongIssuer: return "wrong_issuer";
    case Result::WrongAudience: return "wrong_audience";
    case Result::InsufficientScope: return "insufficient_scope";
    }
    return "unknown";
}

} // namespace jwt_claims
