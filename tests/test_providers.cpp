// Провайдеры, ключи и репозиторий настроек (add-chat-agent, tasks 2.1–2.3).
#include "providers.hpp"

#include "mem_repo.hpp"
#include "test_framework.hpp"

using nlohmann::json;

namespace {

providers::Config cfg(bool llm, bool claude, const std::string& allowed) {
    providers::Config c;
    c.llm_configured = llm;
    c.claude_configured = claude;
    c.claude_allowed = providers::parse_allowed_users(allowed);
    return c;
}

} // namespace

TEST(providers_allowed_users_star_list_empty) {
    auto all = providers::parse_allowed_users("*");
    CHECK(all.all);
    CHECK(all.contains("anyone"));
    auto list = providers::parse_allowed_users(" u-1 , u-2,,u-3 ");
    CHECK(!list.all);
    CHECK(list.contains("u-1"));
    CHECK(list.contains("u-2"));
    CHECK(list.contains("u-3"));
    CHECK(!list.contains("u-4"));
    CHECK(!list.contains(""));
    auto empty = providers::parse_allowed_users("");
    CHECK(!empty.all);
    CHECK(empty.users.empty());
    CHECK(!empty.contains("u-1"));
}

TEST(providers_claude_availability_reasons) {
    providers::Registry no_key(cfg(true, false, "*"));
    auto a = no_key.availability("u-1", providers::kClaude, llm::ProbeResult::Ok);
    CHECK(!a.available);
    CHECK_EQ(a.reason, std::string("not_configured"));

    providers::Registry listed(cfg(true, true, "u-1"));
    CHECK(listed.availability("u-1", providers::kClaude, llm::ProbeResult::Ok).available);
    auto other = listed.availability("u-2", providers::kClaude, llm::ProbeResult::Ok);
    CHECK(!other.available);
    CHECK_EQ(other.reason, std::string("not_allowed"));

    providers::Registry star(cfg(true, true, "*"));
    // Claude не зависит от пробы LM Studio.
    CHECK(star.availability("u-9", providers::kClaude, llm::ProbeResult::Unreachable).available);
    CHECK(!star.availability("u-9", "gpt", llm::ProbeResult::Ok).available);
}

TEST(providers_lmstudio_needs_url_and_ok_probe) {
    providers::Registry reg(cfg(true, false, ""));
    CHECK(reg.availability("u", providers::kLmStudio, llm::ProbeResult::Ok).available);
    auto down = reg.availability("u", providers::kLmStudio, llm::ProbeResult::Unreachable);
    CHECK(!down.available);
    CHECK_EQ(down.reason, std::string("unreachable"));
    CHECK_EQ(reg.availability("u", providers::kLmStudio, llm::ProbeResult::ModelNotLoaded).reason,
             std::string("model_not_loaded"));
    providers::Registry off(cfg(false, false, ""));
    CHECK_EQ(off.availability("u", providers::kLmStudio, llm::ProbeResult::Disabled).reason,
             std::string("not_configured"));
}

TEST(providers_default_must_be_local) {
    providers::Config c = cfg(true, true, "*");
    c.default_provider = providers::kClaude;
    CHECK_EQ(providers::Registry(c).default_provider(), std::string("lmstudio"));
    c.default_provider = "nope";
    CHECK_EQ(providers::Registry(c).default_provider(), std::string("lmstudio"));
}

TEST(providers_credential_not_in_settings_or_log_fields) {
    const std::string key = "sk-ant-secret-123";
    providers::CredentialResolver resolver("lm-secret-456", key);
    auto claude = resolver.resolve("u-1", providers::kClaude);
    REQUIRE(claude.has_value());
    CHECK_EQ(claude->secret, key);
    CHECK(claude->owner == chat::Owner::Instance);
    auto lm = resolver.resolve("u-1", providers::kLmStudio);
    REQUIRE(lm.has_value());
    CHECK(!resolver.resolve("u-1", "gpt").has_value());
    CHECK(!providers::CredentialResolver("", "").resolve("u-1", providers::kClaude).has_value());

    json fields = providers::credential_log_fields(providers::kClaude, *claude);
    CHECK_EQ(fields["owner"].get<std::string>(), std::string("instance"));
    CHECK_EQ(fields["provider"].get<std::string>(), std::string("claude"));
    CHECK(fields.dump().find(key) == std::string::npos);

    providers::Registry reg(cfg(true, true, "*"));
    providers::UserSettings s;
    s.provider = providers::kClaude;
    s.consents[providers::kClaude] = 1793785200;
    std::string body = providers::settings_json(reg, "u-1", s, llm::ProbeResult::Ok).dump();
    CHECK(body.find(key) == std::string::npos);
    CHECK(body.find("lm-secret-456") == std::string::npos);
}

TEST(providers_settings_repo_mem) {
    MemSettingsRepo repo;
    auto empty = repo.get("u-1");
    CHECK(empty.provider.empty());
    CHECK(empty.consents.empty());
    repo.set_consent("u-1", providers::kClaude, 100);
    repo.set_provider("u-1", providers::kClaude);
    repo.set_consent("u-1", providers::kClaude, 200); // повтор — обновляет время
    auto s = repo.get("u-1");
    CHECK_EQ(s.provider, std::string("claude"));
    CHECK_EQ(s.consents.at(providers::kClaude), 200);
    CHECK(repo.get("u-2").provider.empty()); // по пользователю
    repo.revoke_consent("u-1", providers::kClaude);
    CHECK(repo.get("u-1").consents.empty());
    repo.up = false;
    CHECK_THROWS(repo.get("u-1"));
}

TEST(providers_effective_provider) {
    providers::Registry reg(cfg(true, true, "*"));
    providers::UserSettings s;
    CHECK_EQ(providers::effective_provider(reg, s), std::string("lmstudio"));
    s.provider = providers::kClaude; // выбран, но согласия нет — по умолчанию
    CHECK_EQ(providers::effective_provider(reg, s), std::string("lmstudio"));
    s.consents[providers::kClaude] = 1;
    CHECK_EQ(providers::effective_provider(reg, s), std::string("claude"));
    s.provider = "removed-provider";
    CHECK_EQ(providers::effective_provider(reg, s), std::string("lmstudio"));
    CHECK_EQ(providers::iso_utc(1793785200), std::string("2026-11-04T09:40:00Z"));
}
