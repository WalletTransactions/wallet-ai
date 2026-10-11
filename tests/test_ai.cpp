#include "access.hpp"
#include "ai.hpp"
#include "test_framework.hpp"

TEST(required_scope_only_for_api_ai) {
    CHECK_EQ(authz::required_scope("GET", "/api/ai/status"), std::string("ai:use"));
    CHECK_EQ(authz::required_scope("POST", "/api/ai/parse-expense"), std::string("ai:use"));
    CHECK(authz::required_scope("GET", "/healthz").empty());
    CHECK(authz::required_scope("GET", "/metrics").empty());
    CHECK(authz::required_scope("GET", "/docs").empty());
    CHECK(authz::required_scope("GET", "/api/aix").empty());
}

TEST(status_enabled_when_probe_ok) {
    llm::Config cfg;
    cfg.url = "http://h:1234/v1";
    cfg.model = "wallet-parse";
    auto j = aiapi::status_json(cfg, llm::ProbeResult::Ok);
    CHECK(j.at("enabled").get<bool>());
    CHECK_EQ(j.at("model").get<std::string>(), std::string("wallet-parse"));
    CHECK(j.at("reason").is_null());
    CHECK(j.at("features").is_array());
}

TEST(status_disabled_hides_model_and_gives_reason) {
    llm::Config cfg;
    cfg.model = "wallet-parse";
    auto j = aiapi::status_json(cfg, llm::ProbeResult::ModelNotLoaded);
    CHECK(!j.at("enabled").get<bool>());
    CHECK(j.at("model").is_null());
    CHECK_EQ(j.at("reason").get<std::string>(), std::string("model_not_loaded"));

    auto d = aiapi::status_json(llm::Config{}, llm::ProbeResult::Disabled);
    CHECK_EQ(d.at("reason").get<std::string>(), std::string("disabled"));
}
