// Проверка iss/aud/scope access-токена google-auth
// (org/docs/TODO_telegram_tokens.md, шаг 2). Подпись и exp проверяет
// util/jwt — сюда приходит уже проверенный payload.
#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace jwt_claims {

inline constexpr const char* kIssuer = "google-auth";

enum class Result {
    Ok,
    // Токен старого формата — без aud (выпущен до шага 1). Принимается с
    // предупреждением до шага 3 (TODO_telegram_tokens.md), scope не
    // проверяется: у таких токенов его нет, а права у них всегда были полные.
    Legacy,
    WrongIssuer,
    WrongAudience,
    InsufficientScope,
};

// audience — имя ЭТОГО сервиса ("wallet-api", ...). required_scope — пустая
// строка: scope не проверять (только iss/aud).
Result check(const nlohmann::json& payload, const std::string& audience, const std::string& required_scope);

// scope — строка через пробел (RFC 8693/9068); true, если scope в ней есть.
bool has_scope(const nlohmann::json& payload, const std::string& scope);

// Для логов и лейбла метрики: "ok", "legacy", "wrong_issuer", ...
const char* to_string(Result r);

} // namespace jwt_claims
