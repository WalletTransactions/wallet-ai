#include "auth_exchange.hpp"

#include "fake_http.hpp"
#include "nlohmann/json.hpp"
#include "test_framework.hpp"

using nlohmann::json;

namespace {

const char* kSubject = "eyJ.subject-jwt-пользователя.sig";
const char* kIssued = "eyJ.issued-for-wallet-mcp.sig";
const char* kSecret = "s3cr3t-wallet-ai";

std::string ok_body(const std::string& token = kIssued, json expires_in = 120) {
    return json{{"access_token", token},
                {"issued_token_type", auth_exchange::kTokenTypeAccessToken},
                {"token_type", "Bearer"},
                {"expires_in", expires_in}}
        .dump();
}

auth_exchange::Config config(const std::string& url) {
    auth_exchange::Config c;
    c.auth_url = url;
    c.secret = kSecret;
    c.timeout_ms = 3000;
    return c;
}

} // namespace

TEST(auth_exchange_parse_ok) {
    auto r = auth_exchange::parse_exchange_response(200, ok_body());
    REQUIRE(r.status == auth_exchange::Status::Ok);
    CHECK_EQ(r.access_token, std::string(kIssued));
    CHECK_EQ(r.expires_in, 120);
    CHECK(!r.invalid_client);
}

TEST(auth_exchange_parse_400_codes_invalid) {
    for (const char* code : {"invalid_grant", "invalid_target", "invalid_scope", "invalid_request"}) {
        auto r = auth_exchange::parse_exchange_response(400, json{{"error", code}}.dump());
        CHECK(r.status == auth_exchange::Status::Invalid);
        CHECK_EQ(r.error, std::string(code));
        CHECK(!r.invalid_client);
        CHECK(r.access_token.empty());
    }
    CHECK(auth_exchange::parse_exchange_response(400, "не json").status == auth_exchange::Status::Invalid);
}

TEST(auth_exchange_parse_401_invalid_client_is_config_error) {
    auto r = auth_exchange::parse_exchange_response(401, R"({"error":"invalid_client"})");
    CHECK(r.status == auth_exchange::Status::Unavailable);
    CHECK(r.invalid_client);
    // 401 без invalid_client — просто недоступен.
    auto other = auth_exchange::parse_exchange_response(401, "{}");
    CHECK(other.status == auth_exchange::Status::Unavailable);
    CHECK(!other.invalid_client);
}

TEST(auth_exchange_parse_5xx_and_broken_200_unavailable) {
    CHECK(auth_exchange::parse_exchange_response(503, R"({"error":"unavailable"})").status ==
          auth_exchange::Status::Unavailable);
    CHECK(auth_exchange::parse_exchange_response(500, "").status == auth_exchange::Status::Unavailable);
    CHECK(auth_exchange::parse_exchange_response(200, "не json").status == auth_exchange::Status::Unavailable);
    CHECK(auth_exchange::parse_exchange_response(200, R"({"expires_in":120})").status ==
          auth_exchange::Status::Unavailable);
    CHECK(auth_exchange::parse_exchange_response(200, ok_body("")).status == auth_exchange::Status::Unavailable);
    CHECK(auth_exchange::parse_exchange_response(200, ok_body(kIssued, 0)).status ==
          auth_exchange::Status::Unavailable);
    CHECK(auth_exchange::parse_exchange_response(200, ok_body(kIssued, "120")).status ==
          auth_exchange::Status::Unavailable);
}

TEST(auth_exchange_http_sends_rfc8693_form_and_secret) {
    testing::FakeHttpServer srv(auth_exchange::kPath, [](const httplib::Request&, httplib::Response& res) {
        res.set_content(ok_body(), "application/json");
    });
    auto client = auth_exchange::make_http_client(config(srv.origin() + "/"));
    std::string logged;
    auth_exchange::Result r;
    {
        testing::CaptureOutput capture;
        r = client->exchange(kSubject);
        logged = capture.text();
    }
    REQUIRE(r.status == auth_exchange::Status::Ok);
    CHECK_EQ(r.access_token, std::string(kIssued));

    auto reqs = srv.requests();
    REQUIRE(reqs.size() == 1u);
    const auto& req = reqs[0];
    CHECK_EQ(req.get_header_value("X-Service-Secret"), std::string(kSecret));
    CHECK(req.get_header_value("Content-Type").rfind("application/x-www-form-urlencoded", 0) == 0);
    auto form = httplib::Params{};
    httplib::detail::parse_query_text(req.body, form);
    auto field = [&](const char* k) {
        auto it = form.find(k);
        return it == form.end() ? std::string("<нет>") : it->second;
    };
    CHECK_EQ(field("grant_type"), std::string(auth_exchange::kGrantType));
    CHECK_EQ(field("subject_token"), std::string(kSubject));
    CHECK_EQ(field("subject_token_type"), std::string(auth_exchange::kTokenTypeAccessToken));
    CHECK_EQ(field("audience"), std::string("wallet-mcp"));
    CHECK(form.find("scope") == form.end());

    // Ни токены, ни секрет в журнал не пишутся.
    CHECK(logged.find(kSubject) == std::string::npos);
    CHECK(logged.find(kIssued) == std::string::npos);
    CHECK(logged.find(kSecret) == std::string::npos);
}

TEST(auth_exchange_http_invalid_and_invalid_client) {
    testing::FakeHttpServer srv(auth_exchange::kPath, [](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("X-Service-Secret") != kSecret) {
            res.status = 401;
            res.set_content(R"({"error":"invalid_client"})", "application/json");
            return;
        }
        res.status = 400;
        res.set_content(R"({"error":"invalid_grant"})", "application/json");
    });
    auto r = auth_exchange::make_http_client(config(srv.origin()))->exchange(kSubject);
    CHECK(r.status == auth_exchange::Status::Invalid);
    CHECK_EQ(r.error, std::string("invalid_grant"));

    auto cfg = config(srv.origin());
    cfg.secret = "чужой";
    auto bad = auth_exchange::make_http_client(cfg)->exchange(kSubject);
    CHECK(bad.status == auth_exchange::Status::Unavailable);
    CHECK(bad.invalid_client);
    CHECK(bad.error.find(kSubject) == std::string::npos);
}

TEST(auth_exchange_http_network_and_not_configured) {
    auto r = auth_exchange::make_http_client(config("http://127.0.0.1:" + std::to_string(testing::closed_port())))
                 ->exchange(kSubject);
    CHECK(r.status == auth_exchange::Status::Unavailable);
    CHECK(!r.invalid_client);
    CHECK(r.error.rfind("network", 0) == 0);

    auto no_secret = config("http://127.0.0.1:1");
    no_secret.secret.clear();
    auto ns = auth_exchange::make_http_client(no_secret)->exchange(kSubject);
    CHECK(ns.status == auth_exchange::Status::Unavailable);
    CHECK_EQ(ns.error, std::string("secret_not_set"));

    auto no_url = auth_exchange::make_http_client(config(""))->exchange(kSubject);
    CHECK(no_url.status == auth_exchange::Status::Unavailable);
}
