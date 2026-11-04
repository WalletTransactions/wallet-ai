// Обмен JWT пользователя на токен для wallet-mcp (change add-chat-agent, D4):
//
//   POST {AUTH_SERVICE_URL}/internal/token/exchange — RFC 8693, тело
//   application/x-www-form-urlencoded: grant_type (token-exchange),
//   subject_token (JWT пользователя), subject_token_type (access_token),
//   audience=wallet-mcp, без scope — scope выдаётся по таблице политики
//   google-auth (mcp:read). Actor wallet-ai опознаётся по X-Service-Secret
//   (AUTH_EXCHANGE_SECRET). Контракт — operationId exchangeToken в
//   google-auth/openapi.yaml.
//
// Обмен делает хендлер создания задания chat (tasks 5.1): токен пользователя
// там свежий. Выданный токен живёт только во входе задания в памяти.
//
// Клиент сам ничего не пишет в журнал: ни subject_token, ни выданный токен,
// ни секрет не попадают и в Result::error — вызывающий логирует status и error.
#pragma once

#include <memory>
#include <string>

namespace auth_exchange {

inline constexpr const char* kGrantType = "urn:ietf:params:oauth:grant-type:token-exchange";
inline constexpr const char* kTokenTypeAccessToken = "urn:ietf:params:oauth:token-type:access_token";
inline constexpr const char* kAudienceMcp = "wallet-mcp";
inline constexpr const char* kPath = "/internal/token/exchange";

enum class Status {
    Ok,
    // 400 invalid_grant/invalid_target/invalid_scope/invalid_request:
    // google-auth отказал в обмене этого токена — хендлер отвечает 401
    // invalid_token (spec ai-chat, «Токен для wallet-mcp при создании»).
    Invalid,
    // Сеть, таймаут, 5xx, битый 200, 401 invalid_client — хендлер отвечает
    // 503 unavailable.
    Unavailable,
};

const char* to_string(Status s);

struct Result {
    Status status = Status::Unavailable;
    std::string access_token; // только при Ok; в журнал не пишется
    int expires_in = 0;       // секунд, > 0 при Ok
    // 401 {"error":"invalid_client"}: AUTH_EXCHANGE_SECRET не совпадает с
    // секретом wallet-ai в TOKEN_EXCHANGE_SECRETS google-auth — ошибка
    // конфигурации, а не пользователя (status при этом Unavailable).
    bool invalid_client = false;
    // Для журнала: код ошибки google-auth или что не так с ответом. Без
    // токенов и секрета.
    std::string error;
};

// Разбор ответа google-auth — чистая функция:
// 200 с непустым access_token и целым expires_in > 0 — Ok; 400 — Invalid
// (error — код из тела); 401 invalid_client — Unavailable + invalid_client;
// прочее (5xx, 401 без invalid_client, битый 200) — Unavailable.
Result parse_exchange_response(int http_status, const std::string& body);

class Client {
public:
    virtual ~Client() = default;
    // subject_jwt — access-токен пользователя (уже проверенный хендлером).
    virtual Result exchange(const std::string& subject_jwt) = 0;
};

struct Config {
    std::string auth_url;  // AUTH_SERVICE_URL, например http://google-auth:8080
    std::string secret;    // AUTH_EXCHANGE_SECRET
    std::string audience = kAudienceMcp;
    int connect_timeout_ms = 2000;
    int timeout_ms = 5000; // дедлайн на весь запрос
};

// Пустой auth_url или secret — exchange сразу отвечает Unavailable без сети
// (вид chat при этом выключен конфигурацией, design «Migration Plan» п. 2).
std::unique_ptr<Client> make_http_client(Config cfg);

} // namespace auth_exchange
