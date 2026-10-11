// Проверка iss/aud/scope access-токена до хендлеров — тот же приём, что
// access.hpp wallet-report (org/docs/TODO_telegram_tokens.md, шаг 2).
// Отвечает сама, только если токен с верной подписью адресован не этому
// сервису (401) или не даёт scope ai:use (403). Токена нет или подпись битая
// — пропускает: 401 ответит хендлер через session::current_user.
#pragma once

#include <string>

#include "httplib.h"
#include "util/jwks.hpp"

// authz, а не access: access() — POSIX-функция из <unistd.h>, имя занято.
namespace authz {

inline constexpr const char* kAudience = "wallet-ai";
inline constexpr const char* kScopeAi = "ai:use";

// Какой scope нужен запросу. Пусто — эндпоинт без токена (/healthz, /docs, ...).
std::string required_scope(const std::string& method, const std::string& path);

httplib::Server::HandlerResponse gate(const httplib::Request& req, httplib::Response& res, jwks::Cache& jwks_cache);

} // namespace authz
