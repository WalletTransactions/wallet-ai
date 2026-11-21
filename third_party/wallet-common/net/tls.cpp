#include "net/tls.hpp"

#include <cstdlib>
#include <stdexcept>

namespace nettls {

namespace {

std::string env(const char* key) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string();
}

void set_error(std::string* out, std::string msg) {
    if (out) *out = std::move(msg);
}

} // namespace

LoadResult config_from(const std::string& cert, const std::string& key, const std::string& ca) {
    LoadResult r;
    if (cert.empty() && key.empty() && ca.empty()) return r; // выключено
    std::string missing;
    if (cert.empty()) missing += (missing.empty() ? "" : ", ") + std::string("TLS_CERT");
    if (key.empty()) missing += (missing.empty() ? "" : ", ") + std::string("TLS_KEY");
    if (ca.empty()) missing += (missing.empty() ? "" : ", ") + std::string("TLS_CA");
    if (!missing.empty()) {
        r.error = "TLS задан не полностью: не хватает " + missing +
                  " (нужны все три или ни одной)";
        return r;
    }
    r.config = Config{cert, key, ca};
    return r;
}

LoadResult config_from_env() { return config_from(env("TLS_CERT"), env("TLS_KEY"), env("TLS_CA")); }

Settings settings_from_env() {
    Settings s;
    auto loaded = config_from_env();
    if (!loaded.error.empty()) {
        s.error = loaded.error;
        return s;
    }
    s.config = loaded.config;
    const std::string full = env("INTERNAL_MTLS");
    if (!full.empty() && full != "true" && full != "false") {
        s.error = "INTERNAL_MTLS должен быть true или false, получено: " + full;
        return s;
    }
    if (full == "true") {
        if (!s.config.enabled()) {
            s.error = "INTERNAL_MTLS=true, но не заданы TLS_CERT, TLS_KEY, TLS_CA";
            return s;
        }
        s.serve = true;
        s.call_internal = true;
    }
    return s;
}

httplib::Client client_value(const std::string& base_url, const Config& cfg) {
    std::string err;
    auto client = make_client(base_url, cfg, &err);
    if (!client) throw std::runtime_error(err.empty() ? "TLS-клиент не создан" : err);
    return std::move(*client);
}

std::string to_https(const std::string& base_url) {
    static const std::string kHttp = "http://";
    if (base_url.rfind(kHttp, 0) == 0) return "https://" + base_url.substr(kHttp.size());
    return base_url;
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT

std::unique_ptr<httplib::Server> make_server(const Config& cfg, std::string* error) {
    if (!cfg.enabled()) return std::make_unique<httplib::Server>();
    auto svr = std::make_unique<httplib::SSLServer>(cfg.cert.c_str(), cfg.key.c_str(), cfg.ca.c_str());
    if (!svr->is_valid()) {
        set_error(error, "TLS-сервер не создан: проверьте TLS_CERT=" + cfg.cert + ", TLS_KEY=" + cfg.key +
                             ", TLS_CA=" + cfg.ca + " (файл не читается или ключ не от этого сертификата)");
        return nullptr;
    }
    return svr;
}

std::unique_ptr<httplib::Client> make_client(const std::string& base_url, const Config& cfg,
                                             std::string* error) {
    if (!cfg.enabled()) return std::make_unique<httplib::Client>(base_url);
    auto cli = std::make_unique<httplib::Client>(to_https(base_url), cfg.cert, cfg.key);
    if (!cli->is_valid()) {
        set_error(error, "TLS-клиент не создан для " + base_url + ": проверьте TLS_CERT=" + cfg.cert +
                             ", TLS_KEY=" + cfg.key);
        return nullptr;
    }
    cli->set_ca_cert_path(cfg.ca);
    cli->enable_system_ca(false); // доверяем только CA контура, не системным корням
    cli->enable_server_certificate_verification(true);
    cli->enable_server_hostname_verification(true);
    return cli;
}

std::string peer_service(const httplib::Request& req) {
    if (!req.ssl) return std::string();
    return req.peer_cert().subject_cn();
}

#else // без OpenSSL: режим выключен работает, включённый — отказ

std::unique_ptr<httplib::Server> make_server(const Config& cfg, std::string* error) {
    if (!cfg.enabled()) return std::make_unique<httplib::Server>();
    set_error(error, "TLS включён, но сервис собран без CPPHTTPLIB_OPENSSL_SUPPORT");
    return nullptr;
}

std::unique_ptr<httplib::Client> make_client(const std::string& base_url, const Config& cfg,
                                             std::string* error) {
    if (!cfg.enabled()) return std::make_unique<httplib::Client>(base_url);
    set_error(error, "TLS включён, но сервис собран без CPPHTTPLIB_OPENSSL_SUPPORT");
    return nullptr;
}

std::string peer_service(const httplib::Request&) { return std::string(); }

#endif

} // namespace nettls
