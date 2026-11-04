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

// --- Чат и настройки ИИ (add-chat-agent, tasks 2.4, 5.1) ---

#include "mem_repo.hpp"

using nlohmann::json;

namespace {

const char* kU1 = "11111111-1111-4111-8111-111111111111";
const char* kU2 = "22222222-2222-4222-8222-222222222222";

class StubModel : public chat::Model {
public:
    chat::Turn turn(const std::string&, const std::vector<chat::Message>&, const std::vector<mcp::Tool>&, bool,
                    const std::optional<chat::Credential>&, int) override {
        return {};
    }
};

class FakeExchange : public auth_exchange::Client {
public:
    auth_exchange::Result next;
    int calls = 0;
    std::string subject;
    auth_exchange::Result exchange(const std::string& subject_jwt) override {
        ++calls;
        subject = subject_jwt;
        return next;
    }
};

// Состояние сервиса без сети: реестр (Claude — только для kU1), настройки в
// памяти, фейковый обмен токена.
struct ChatState {
    AppState state;
    FakeExchange* exchange = nullptr;
    MemSettingsRepo* repo = nullptr;

    ChatState() {
        state.jobs = std::make_unique<jobs::Store>(jobs::Limits{});
        providers::Config c;
        c.llm_configured = true;
        c.claude_configured = true;
        c.claude_allowed = providers::parse_allowed_users(kU1);
        state.providers = std::make_unique<providers::Registry>(c);
        state.providers->set_model(providers::kLmStudio, std::make_shared<StubModel>());
        state.providers->set_model(providers::kClaude, std::make_shared<StubModel>());
        auto ex = std::make_unique<FakeExchange>();
        ex->next.status = auth_exchange::Status::Ok;
        ex->next.access_token = "mcp-token-secret";
        ex->next.expires_in = 300;
        exchange = ex.get();
        state.auth_exchange = std::move(ex);
        auto r = std::make_unique<MemSettingsRepo>();
        repo = r.get();
        state.settings_repo = std::move(r);
        state.db_up.store(true);
        state.chat_enabled = true;
        state.llm_status.store(llm::ProbeResult::Ok);
    }

    providers::Reply create(const json& input, const std::string& user = kU1) {
        return aiapi::create_chat_job(state, session::User{user, "telegram", "user-jwt"}, input, "");
    }
    providers::Reply get(const std::string& user = kU1) {
        return providers::get_settings(*state.providers, aiapi::settings_repo(state), user, state.llm_status.load());
    }
    providers::Reply put(const json& body, const std::string& user = kU1) {
        return providers::put_settings(*state.providers, aiapi::settings_repo(state), user, body,
                                       state.llm_status.load(), 1793785200);
    }
};

const json* provider_item(const json& settings, const std::string& id) {
    for (const auto& p : settings["providers"]) {
        if (p["id"] == id) return &p;
    }
    return nullptr;
}

} // namespace

TEST(status_lists_chat_only_when_configured) {
    llm::Config cfg;
    auto on = aiapi::status_json(cfg, llm::ProbeResult::Ok, false, true);
    CHECK_EQ(on["features"], json({"parse_expense", "chat"}));
    CHECK_EQ(aiapi::status_json(cfg, llm::ProbeResult::Ok)["features"], json({"parse_expense"}));
}

TEST(settings_new_user_defaults) {
    ChatState s;
    auto r = s.get();
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body["provider"].get<std::string>(), std::string("lmstudio"));
    const json* claude = provider_item(r.body, "claude");
    REQUIRE(claude != nullptr);
    CHECK_EQ((*claude)["consented"].get<bool>(), false);
    CHECK((*claude)["cloud"].get<bool>());
    CHECK((*claude)["requires_consent"].get<bool>());
    CHECK((*claude)["available"].get<bool>());
    CHECK(!claude->contains("reason"));
    auto r2 = s.get(kU2);
    const json* other = provider_item(r2.body, "claude");
    CHECK_EQ((*other)["reason"].get<std::string>(), std::string("not_allowed"));
}

TEST(settings_put_consent_flow) {
    ChatState s;
    // Без согласия — 409, выбор не меняется.
    auto no = s.put({{"provider", "claude"}});
    CHECK_EQ(no.status, 409);
    CHECK_EQ(no.body["error"].get<std::string>(), std::string("consent_required"));
    CHECK_EQ(s.get().body["provider"].get<std::string>(), std::string("lmstudio"));

    // С согласием — 200, согласие и время сохранены.
    auto yes = s.put({{"provider", "claude"}, {"consent", true}});
    CHECK_EQ(yes.status, 200);
    CHECK_EQ(yes.body["provider"].get<std::string>(), std::string("claude"));
    const json* claude = provider_item(yes.body, "claude");
    CHECK((*claude)["consented"].get<bool>());
    CHECK_EQ((*claude)["consented_at"].get<std::string>(), std::string("2026-11-04T09:40:00Z"));

    // Повторный выбор без consent — согласие уже есть.
    CHECK_EQ(s.put({{"provider", "lmstudio"}}).status, 200);
    CHECK_EQ(s.put({{"provider", "claude"}}).status, 200);

    // Отзыв: согласие стёрто, выбор — lmstudio.
    auto revoke = s.put({{"provider", "claude"}, {"consent", false}});
    CHECK_EQ(revoke.status, 200);
    CHECK_EQ(revoke.body["provider"].get<std::string>(), std::string("lmstudio"));
    CHECK_EQ((*provider_item(revoke.body, "claude"))["consented"].get<bool>(), false);
    CHECK(s.repo->consent.empty());
}

TEST(settings_put_rejections) {
    ChatState s;
    CHECK_EQ(s.put({{"provider", "gpt"}}).body["error"].get<std::string>(), std::string("provider_unavailable"));
    CHECK_EQ(s.put({{"provider", "claude"}, {"consent", true}}, kU2).status, 400); // не в списке
    CHECK_EQ(s.put(json::array()).status, 400);
    CHECK_EQ(s.put({{"provider", "claude"}, {"consent", "yes"}}).status, 400);
    s.state.llm_status.store(llm::ProbeResult::Unreachable);
    CHECK_EQ(s.put({{"provider", "lmstudio"}}).body["error"].get<std::string>(), std::string("provider_unavailable"));

    // Без базы: PUT — 503, GET — провайдер по умолчанию.
    s.state.db_up.store(false);
    auto put = s.put({{"provider", "claude"}, {"consent", true}});
    CHECK_EQ(put.status, 503);
    CHECK_EQ(put.body["error"].get<std::string>(), std::string("settings_unavailable"));
    CHECK_EQ(s.get().status, 200);
    CHECK_EQ(s.get().body["provider"].get<std::string>(), std::string("lmstudio"));
}

TEST(chat_create_validation_and_disabled) {
    ChatState s;
    CHECK_EQ(s.create({{"text", ""}}).status, 400);
    CHECK_EQ(s.create({{"text", std::string(1001, 'x')}}).status, 400);
    CHECK_EQ(s.exchange->calls, 0);

    s.state.chat_enabled = false; // нет WALLET_MCP_URL или AUTH_EXCHANGE_SECRET
    auto r = s.create({{"text", "сколько за няню"}});
    CHECK_EQ(r.status, 503);
    CHECK_EQ(r.body, json({{"error", "unavailable"}}));
    CHECK_EQ(s.exchange->calls, 0); // google-auth не вызывается
}

TEST(chat_create_lmstudio_unavailable_no_silent_switch) {
    ChatState s;
    s.state.llm_status.store(llm::ProbeResult::Unreachable);
    auto r = s.create({{"text", "сколько за няню"}});
    CHECK_EQ(r.status, 503);
    CHECK_EQ(r.body, json({{"error", "provider_unavailable"}, {"provider", "lmstudio"}}));
    CHECK_EQ(s.exchange->calls, 0);
}

TEST(chat_create_claude_cloud_lane_when_lm_down) {
    ChatState s;
    REQUIRE(s.put({{"provider", "claude"}, {"consent", true}}).status == 200);
    s.state.llm_status.store(llm::ProbeResult::Unreachable);
    auto r = s.create({{"text", "сколько за няню"}, {"conversation_id", "tg-42-1"}});
    REQUIRE(r.status == 202);
    CHECK_EQ(r.body["status"].get<std::string>(), std::string("queued"));
    CHECK_EQ(s.exchange->calls, 1);
    CHECK_EQ(s.exchange->subject, std::string("user-jwt"));
    CHECK_EQ(s.state.jobs->queued(jobs::Priority::Interactive, jobs::Lane::Cloud), 1u);
    CHECK_EQ(s.state.jobs->queued(jobs::Priority::Interactive, jobs::Lane::Local), 0u);

    // Токен для wallet-mcp — только во входе для воркера, не в снимке.
    std::string id = r.body["job_id"].get<std::string>();
    auto snap = s.state.jobs->get(id, kU1);
    REQUIRE(snap.has_value());
    CHECK(snap->input.is_null());
    CHECK(aiapi::job_json(*snap).dump().find("mcp-token-secret") == std::string::npos);
    auto taken = s.state.jobs->take(jobs::Lane::Cloud);
    REQUIRE(taken.has_value());
    CHECK_EQ(taken->input["mcp_token"].get<std::string>(), std::string("mcp-token-secret"));
    CHECK_EQ(taken->input["provider"].get<std::string>(), std::string("claude"));
    CHECK_EQ(taken->input["conversation_id"].get<std::string>(), std::string("tg-42-1"));
    s.state.jobs->complete(id, jobs::Status::Done, json{{"text", "ok"}}, "");
    auto done = s.state.jobs->get(id, kU1);
    CHECK(aiapi::job_json(*done).dump().find("mcp-token-secret") == std::string::npos);
}

TEST(chat_create_exchange_failures_and_new_conversation) {
    ChatState s;
    s.exchange->next = {};
    s.exchange->next.status = auth_exchange::Status::Invalid;
    s.exchange->next.error = "invalid_grant";
    auto bad = s.create({{"text", "x"}});
    CHECK_EQ(bad.status, 401);
    CHECK_EQ(bad.body, json({{"error", "invalid_token"}}));

    s.exchange->next.status = auth_exchange::Status::Unavailable;
    s.exchange->next.invalid_client = true;
    auto down = s.create({{"text", "x"}});
    CHECK_EQ(down.status, 503);
    CHECK_EQ(down.body, json({{"error", "unavailable"}}));
    CHECK_EQ(s.state.jobs->queued(jobs::Priority::Interactive), 0u); // заданий не создано

    s.exchange->next = {};
    s.exchange->next.status = auth_exchange::Status::Ok;
    s.exchange->next.access_token = "t";
    auto ok = s.create({{"text", "x"}});
    REQUIRE(ok.status == 202);
    auto taken = s.state.jobs->take(jobs::Lane::Local); // lmstudio — локальная полоса
    REQUIRE(taken.has_value());
    CHECK_EQ(taken->kind, std::string("chat"));
    CHECK_EQ(taken->input["conversation_id"].get<std::string>().size(), 36u); // новый UUID
}
