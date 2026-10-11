// Клиент wallet-report: бот и wallet-ai читают кошельки, балансы и операции
// теми же эндпоинтами, что веб-клиент, с access-токеном пользователя (Bearer).
// Ходят напрямую на wallet-report по внутренней сети — тот сам проверяет JWT
// по JWKS google-auth.
#pragma once

#include <memory>
#include <string>

#include "nlohmann/json.hpp"

namespace reports {

struct Response {
    int status = 0; // 0 — до HTTP-ответа не дошло (сеть, таймаут)
    nlohmann::json body;
    std::string error; // для лога
};

class ReportsApi {
public:
    virtual ~ReportsApi() = default;
    // path_and_query — например "/api/reports/accounts?kind=wallet";
    // значения query вызывающий экранирует сам (url_encode ниже).
    virtual Response get(const std::string& path_and_query, const std::string& access_token) = 0;
};

// Percent-encoding значения query-параметра (RFC 3986 unreserved остаются).
std::string url_encode(const std::string& value);

// error_metric — счётчик ошибок с лейблом kind (network, http_<код>): у
// каждого сервиса свой (бот — tg_reports_errors_total, он в дашборде).
std::unique_ptr<ReportsApi> make_http_api(const std::string& base_url, const std::string& error_metric);

} // namespace reports
