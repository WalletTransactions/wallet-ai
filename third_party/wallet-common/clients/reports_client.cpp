#include "clients/reports_client.hpp"

#include <cctype>
#include <cstdio>

#include "httplib.h"
#include "util/log.hpp"
#include "util/metrics.hpp"

using nlohmann::json;

namespace reports {

namespace {

class HttpReportsApi final : public ReportsApi {
public:
    HttpReportsApi(const std::string& base_url, const std::string& error_metric)
        : base_url_(base_url), error_metric_(error_metric) {}

    Response get(const std::string& path_and_query, const std::string& access_token) override {
        // Клиент на вызов: запросов мало, а у httplib::Client нет гарантий на
        // переиспользование после ошибок.
        httplib::Client client(base_url_);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(15, 0);

        httplib::Headers headers = {{"Authorization", "Bearer " + access_token}};
        std::string trace_id = applog::current_trace_id();
        if (!trace_id.empty()) headers.emplace("X-Trace-Id", trace_id);

        Response out;
        auto res = client.Get(path_and_query, headers);
        if (!res) {
            metrics::inc_counter(error_metric_, "Ошибки запросов к wallet-report.", "kind", "network");
            out.error = "wallet-report недоступен: " + httplib::to_string(res.error());
            return out;
        }
        out.status = res->status;
        out.body = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
        if (res->status != 200) {
            metrics::inc_counter(error_metric_, "Ошибки запросов к wallet-report.", "kind",
                                 "http_" + std::to_string(res->status));
            out.error = "HTTP " + std::to_string(res->status);
            if (out.body.is_object() && out.body.contains("error") && out.body["error"].is_string()) {
                out.error += ": " + out.body["error"].get<std::string>();
            }
        } else if (out.body.is_discarded()) {
            out.status = 0;
            out.error = "ответ wallet-report не JSON";
        }
        return out;
    }

private:
    std::string base_url_;
    std::string error_metric_;
};

} // namespace

std::string url_encode(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::unique_ptr<ReportsApi> make_http_api(const std::string& base_url, const std::string& error_metric) {
    return std::make_unique<HttpReportsApi>(base_url, error_metric);
}

} // namespace reports
