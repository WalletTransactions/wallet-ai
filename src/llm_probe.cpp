#include "llm_probe.hpp"

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace llm {

BaseUrl split_base_url(const std::string& url) {
    std::string u = url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    auto scheme_end = u.find("://");
    if (scheme_end == std::string::npos || scheme_end + 3 >= u.size()) return {};
    auto path_start = u.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return {u, ""};
    return {u.substr(0, path_start), u.substr(path_start)};
}

const char* to_string(ProbeResult r) {
    switch (r) {
    case ProbeResult::Ok: return "ok";
    case ProbeResult::Disabled: return "disabled";
    case ProbeResult::Unreachable: return "unreachable";
    case ProbeResult::Unauthorized: return "unauthorized";
    case ProbeResult::ModelNotLoaded: return "model_not_loaded";
    case ProbeResult::BadResponse: return "bad_response";
    }
    return "unknown";
}

ProbeResult parse_models_response(int status, const std::string& body, const std::string& model) {
    if (status == 401 || status == 403) return ProbeResult::Unauthorized;
    if (status < 200 || status >= 300) return ProbeResult::Unreachable;
    try {
        auto j = nlohmann::json::parse(body);
        const auto& data = j.at("data");
        if (!data.is_array()) return ProbeResult::BadResponse;
        for (const auto& m : data) {
            if (m.value("id", std::string{}) == model) return ProbeResult::Ok;
        }
        return ProbeResult::ModelNotLoaded;
    } catch (const std::exception&) {
        return ProbeResult::BadResponse;
    }
}

namespace {

class HttpProbe : public Probe {
public:
    explicit HttpProbe(Config cfg) : cfg_(std::move(cfg)), base_(split_base_url(cfg_.url)) {}

    ProbeResult probe() override {
        if (cfg_.url.empty()) return ProbeResult::Disabled;
        if (base_.origin.empty()) return ProbeResult::Unreachable;

        // Клиент на вызов: проба раз в минуту, держать соединение незачем.
        httplib::Client cli(base_.origin);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(cfg_.timeout_ms / 1000, (cfg_.timeout_ms % 1000) * 1000);
        httplib::Headers headers;
        if (!cfg_.api_key.empty()) headers.emplace("Authorization", "Bearer " + cfg_.api_key);

        auto res = cli.Get(base_.path + "/models", headers);
        if (!res) return ProbeResult::Unreachable;
        return parse_models_response(res->status, res->body, cfg_.model);
    }

private:
    Config cfg_;
    BaseUrl base_;
};

} // namespace

std::unique_ptr<Probe> make_http_probe(const Config& cfg) { return std::make_unique<HttpProbe>(cfg); }

} // namespace llm
