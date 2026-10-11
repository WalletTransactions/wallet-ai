// Минимальная реализация JWT (JWS Compact Serialization) поверх
// util/crypto.hpp + nlohmann::json — без внешней JWT-библиотеки, чтобы не
// тащить лишнюю зависимость ради десятка строк кодирования/проверки.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "util/crypto.hpp"

namespace jwt {

// Подписывает payload как RS256 JWT (для СВОИХ access-токенов google-auth) —
// заголовок получает "kid" из key.kid(), верифицирующая сторона использует
// его, чтобы выбрать нужный публичный ключ из JWKS (GET /.well-known/jwks.json).
std::string sign_rs256(const nlohmann::json& payload, const crypto::RsaSigningKey& key);

enum class VerifyError {
    Malformed,     // не три части, битый base64url или JSON
    BadSignature,  // подпись не сошлась с ключом
    Expired,       // exp нет, он не число или уже наступил
};

// Проверяет подпись RS256 по сырым модулю/экспоненте RSA-ключа (как их
// отдаёт JWKS) и срок действия. exp ОБЯЗАТЕЛЕН: токен без него не принимается
// — иначе он был бы бессрочным. НЕ проверяет остальные claims (iss/aud/scope)
// — см. util/jwt_claims.hpp. Возвращает payload при успехе, иначе nullopt и
// причину в *error, если он передан (google-auth различает "id_token истёк").
std::optional<nlohmann::json> verify_rs256(const std::string& token,
                                            const std::vector<unsigned char>& n,
                                            const std::vector<unsigned char>& e,
                                            VerifyError* error = nullptr);

// Читает `kid` из заголовка БЕЗ проверки подписи — нужен только чтобы понять,
// каким ключом из JWKS проверять токен дальше.
std::optional<std::string> peek_kid(const std::string& token);

} // namespace jwt
