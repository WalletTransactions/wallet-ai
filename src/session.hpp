// Проверка access-токена google-auth: Bearer или cookie "session", подпись по
// JWKS (util/jwks.hpp) и exp. Тот же код, что session.hpp wallet-report.
#pragma once

#include <optional>
#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "util/jwks.hpp"

namespace session {

struct User {
    std::string user_id;
    // "web"/"android"/"telegram"/... — откуда запрос (метрики по платформе,
    // TODO_wallet_ai.md, §9). Пусто у токена браузерного входа.
    std::string platform;
    // Сырой токен запроса — wallet-ai ходит с ним в wallet-report от имени
    // пользователя (TODO_wallet_ai.md, §1), своего доступа к данным у него нет.
    std::string token;
};

// nullopt — токена нет, kid неизвестен, подпись не сошлась или токен
// просрочен. Вызывающий код отвечает 401 во всех случаях.
std::optional<User> current_user(const httplib::Request& req, jwks::Cache& jwks_cache);

// Payload после проверки подписи и exp — для проверки iss/aud/scope (access.hpp).
std::optional<nlohmann::json> verified_payload(const httplib::Request& req, jwks::Cache& jwks_cache);

// Bearer, иначе cookie "session"; пусто — токена нет.
std::string extract_token(const httplib::Request& req);

} // namespace session
