#include "auth_exchange.hpp"

#include <utility>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "util/log.hpp"

using nlohmann::json;

namespace auth_exchange {

const char* to_string(Status s) {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::Invalid: return "invalid";
    case Status::Unavailable: return "unavailable";
    }
    return "unavailable";
}

namespace {

// {"error":"<код>"} из тела ошибки google-auth; не JSON — пусто.
std::string error_code(const std::string& body) {
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_object() && j.contains("error") && j["error"].is_string()) return j["error"].get<std::string>();
    return {};
}

} // namespace

Result parse_exchange_response(int http_status, const std::string& body) {
    Result out;
    if (http_status == 200) {
        json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object() || !j.contains("access_token") || !j["access_token"].is_string() ||
            !j.contains("expires_in") || !j["expires_in"].is_number_integer()) {
            out.error = "bad_response";
            return out;
        }
        std::string token = j["access_token"].get<std::string>();
        long long expires_in = j["expires_in"].get<long long>();
        if (token.empty() || expires_in <= 0) {
            out.error = "bad_response";
            return out;
        }
        out.status = Status::Ok;
        out.access_token = std::move(token);
        // google-auth выдаёт не дольше 300 с; ограничение — от переполнения int.
        out.expires_in = expires_in > 86400 ? 86400 : static_cast<int>(expires_in);
        return out;
    }
    std::string code = error_code(body);
    if (http_status == 400) {
        out.status = Status::Invalid;
        out.error = code.empty() ? "http_400" : code;
        return out;
    }
    if (http_status == 401 && code == "invalid_client") {
        out.invalid_client = true;
        out.error = "invalid_client";
        return out;
    }
    out.error = "http_" + std::to_string(http_status) + (code.empty() ? "" : " " + code);
    return out;
}

namespace {

class HttpClient final : public Client {
public:
    explicit HttpClient(Config cfg) : cfg_(std::move(cfg)) {
        while (!cfg_.auth_url.empty() && cfg_.auth_url.back() == '/') cfg_.auth_url.pop_back();
    }

    Result exchange(const std::string& subject_jwt) override {
        Result out;
        if (cfg_.auth_url.empty()) {
            out.error = "auth_url_not_set";
            return out;
        }
        if (cfg_.secret.empty()) {
            out.error = "secret_not_set";
            return out;
        }
        // Клиент на вызов: обмен — раз на задание chat.
        httplib::Client cli(cfg_.auth_url);
        cli.set_connection_timeout(cfg_.connect_timeout_ms / 1000, (cfg_.connect_timeout_ms % 1000) * 1000);
        cli.set_read_timeout(cfg_.timeout_ms / 1000, (cfg_.timeout_ms % 1000) * 1000);
        cli.set_write_timeout(cfg_.timeout_ms / 1000, (cfg_.timeout_ms % 1000) * 1000);
        cli.set_max_timeout(cfg_.timeout_ms);

        httplib::Headers headers;
        std::string trace_id = applog::current_trace_id();
        if (!trace_id.empty()) headers.emplace("X-Trace-Id", trace_id);
        headers.emplace("X-Service-Secret", cfg_.secret);
        // Без scope: google-auth выдаёт scope из своей таблицы политики.
        httplib::Params form{{"grant_type", kGrantType},
                             {"subject_token", subject_jwt},
                             {"subject_token_type", kTokenTypeAccessToken},
                             {"audience", cfg_.audience}};
        auto res = cli.Post(kPath, headers, form);
        if (!res) {
            out.error = std::string("network: ") + httplib::to_string(res.error());
            return out;
        }
        return parse_exchange_response(res->status, res->body);
    }

private:
    Config cfg_;
};

} // namespace

std::unique_ptr<Client> make_http_client(Config cfg) { return std::make_unique<HttpClient>(std::move(cfg)); }

} // namespace auth_exchange
