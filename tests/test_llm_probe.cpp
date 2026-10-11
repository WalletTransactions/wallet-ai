#include "llm_probe.hpp"

#include "test_framework.hpp"

TEST(split_base_url_with_path) {
    auto b = llm::split_base_url("http://host.docker.internal:1234/v1");
    CHECK_EQ(b.origin, std::string("http://host.docker.internal:1234"));
    CHECK_EQ(b.path, std::string("/v1"));
}

TEST(split_base_url_trailing_slash_and_no_path) {
    auto b = llm::split_base_url("http://127.0.0.1:1234/v1/");
    CHECK_EQ(b.path, std::string("/v1"));
    auto c = llm::split_base_url("http://llama:8080");
    CHECK_EQ(c.origin, std::string("http://llama:8080"));
    CHECK_EQ(c.path, std::string(""));
}

TEST(split_base_url_rejects_garbage) {
    CHECK(llm::split_base_url("host:1234/v1").origin.empty());
    CHECK(llm::split_base_url("http://").origin.empty());
    CHECK(llm::split_base_url("").origin.empty());
}

TEST(parse_models_finds_model) {
    std::string body = R"({"data":[{"id":"wallet-parse","object":"model"},{"id":"google/gemma-4-e4b"}],"object":"list"})";
    CHECK(llm::parse_models_response(200, body, "wallet-parse") == llm::ProbeResult::Ok);
    CHECK(llm::parse_models_response(200, body, "qwen3-8b") == llm::ProbeResult::ModelNotLoaded);
}

TEST(parse_models_errors) {
    CHECK(llm::parse_models_response(401, "", "m") == llm::ProbeResult::Unauthorized);
    CHECK(llm::parse_models_response(403, "", "m") == llm::ProbeResult::Unauthorized);
    CHECK(llm::parse_models_response(500, "", "m") == llm::ProbeResult::Unreachable);
    CHECK(llm::parse_models_response(200, "not json", "m") == llm::ProbeResult::BadResponse);
    CHECK(llm::parse_models_response(200, R"({"data":{}})", "m") == llm::ProbeResult::BadResponse);
    CHECK(llm::parse_models_response(200, R"({"object":"list"})", "m") == llm::ProbeResult::BadResponse);
}

TEST(http_probe_disabled_without_url) {
    auto p = llm::make_http_probe(llm::Config{});
    CHECK(p->probe() == llm::ProbeResult::Disabled);
}
