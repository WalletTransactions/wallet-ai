// Задание chat (add-chat-agent, tasks 5.2–5.5): агентный цикл на FakeModel и
// фейковом wallet-mcp, история диалога, промпт, метрики и журнал.
#include "chat.hpp"

#include <functional>

#include "chat_metrics.hpp"
#include "fake_http.hpp"
#include "test_framework.hpp"

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

const char* kUser = "11111111-1111-4111-8111-111111111111";
const char* kOther = "22222222-2222-4222-8222-222222222222";
const char* kQuestion = "сколько я потратил на няню в сентябре?";

// Модель по сценарию: step(i, messages, allow_tools) → ход. Запоминает, что
// ей передали.
class FakeModel : public chat::Model {
public:
    using Step = std::function<chat::Turn(int, const std::vector<chat::Message>&, bool)>;
    explicit FakeModel(Step step) : step_(std::move(step)) {}

    chat::Turn turn(const std::string& system, const std::vector<chat::Message>& messages,
                    const std::vector<mcp::Tool>& tools, bool allow_tools,
                    const std::optional<chat::Credential>& credential, int timeout_ms) override {
        systems.push_back(system);
        calls.push_back(messages);
        allow.push_back(allow_tools);
        tool_counts.push_back(tools.size());
        credentials.push_back(credential);
        timeouts.push_back(timeout_ms);
        return step_(static_cast<int>(calls.size()) - 1, messages, allow_tools);
    }

    std::vector<std::string> systems;
    std::vector<std::vector<chat::Message>> calls;
    std::vector<bool> allow;
    std::vector<std::size_t> tool_counts;
    std::vector<std::optional<chat::Credential>> credentials;
    std::vector<int> timeouts;

private:
    Step step_;
};

// wallet-mcp без сети.
class FakeMcp : public mcp::Client {
public:
    mcp::Status init_status = mcp::Status::Ok;
    mcp::Status list_status = mcp::Status::Ok;
    std::function<mcp::CallResult(const std::string&, const json&)> on_call;
    std::vector<std::pair<std::string, json>> called;
    std::string seen_token;
    int inits = 0;

    mcp::InitResult initialize(mcp::Session& session, int) override {
        ++inits;
        seen_token = session.token;
        mcp::InitResult r;
        r.status = init_status;
        r.instructions = "Сегодня 2026-11-04. Периоды — включительно.";
        r.protocol_version = mcp::kProtocolVersion;
        if (r.status != mcp::Status::Ok) r.detail = "initialize: http_503";
        return r;
    }
    mcp::ToolsResult list_tools(const mcp::Session&, int) override {
        mcp::ToolsResult r;
        r.status = list_status;
        if (r.status != mcp::Status::Ok) {
            r.detail = "tools/list: network";
            return r;
        }
        for (const char* n : {"spending_by_category", "list_categories", "search_operations"}) {
            mcp::Tool t;
            t.name = n;
            t.description = n;
            r.tools.push_back(t);
        }
        return r;
    }
    mcp::CallResult call_tool(const mcp::Session&, const std::string& name, const json& args, std::size_t,
                              int) override {
        called.push_back({name, args});
        if (on_call) return on_call(name, args);
        mcp::CallResult r;
        r.status = mcp::Status::Ok;
        r.content = "Няня: 42000 RUB";
        return r;
    }
};

chat::Turn tool_turn(const std::string& name, json args, const std::string& id, int in = 100, int out = 10) {
    chat::Turn t;
    t.kind = chat::TurnKind::ToolCalls;
    t.message.role = chat::Role::Assistant;
    t.message.tool_calls.push_back({id, name, std::move(args), true});
    t.usage = {in, out};
    return t;
}

chat::Turn text_turn(const std::string& text, int in = 100, int out = 10) {
    chat::Turn t;
    t.kind = chat::TurnKind::Text;
    t.message.role = chat::Role::Assistant;
    t.message.text = text;
    t.usage = {in, out};
    return t;
}

chat::Turn error_turn(chat::ModelError e) {
    chat::Turn t;
    t.kind = chat::TurnKind::Error;
    t.error = e;
    t.detail = "http_401";
    return t;
}

// Стенд: реестр с одним провайдером, фейки и ручные часы.
struct Bench {
    std::shared_ptr<FakeModel> model;
    FakeMcp mcp;
    providers::Registry registry{providers::Config{}};
    providers::CredentialResolver credentials{"", "sk-ant-secret"};
    chat::Conversations conversations{6, 1800s};
    chat::Agent agent;
    std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::time_point{} + 1000h;
    std::string provider;

    explicit Bench(FakeModel::Step step, const std::string& provider_id = providers::kLmStudio)
        : model(std::make_shared<FakeModel>(std::move(step))), provider(provider_id) {
        registry.set_model(provider, model);
        agent.mcp = &mcp;
        agent.registry = &registry;
        agent.credentials = &credentials;
        agent.conversations = &conversations;
        agent.prompt = llm::parse_prompt("# version: chat-test\nСегодня {today}, {weekday}, {time}.\n{mcp_instructions}");
        agent.now = [this] { return clock; };
        agent.wall_now = [] { return std::time_t{1793785200}; };
    }

    jobs::Job job(const std::string& text, const std::string& conversation = "tg-42-1",
                  const std::string& user = kUser) {
        jobs::Job j;
        j.id = "33333333-3333-4333-8333-333333333333";
        j.user_id = user;
        j.kind = chat::kKind;
        json client = {{"text", text}, {"now", {{"date", "2026-11-04"}, {"time", "14:05:00"}}}};
        j.input = chat::make_job_input(client, conversation, provider, "mcp-token-xyz");
        return j;
    }
};

} // namespace

TEST(chat_input_validation) {
    CHECK(chat::validate_input({{"text", "сколько за няню"}, {"conversation_id", "tg-42-1"}}).empty());
    CHECK(chat::validate_input({{"text", "x"}, {"now", {{"date", "2026-11-04"}, {"time", "14:05:00"}}}}).empty());
    CHECK(!chat::validate_input({{"text", ""}}).empty());
    CHECK(!chat::validate_input({{"text", "   "}}).empty());
    CHECK(!chat::validate_input(json::object()).empty());
    CHECK(!chat::validate_input(json::array()).empty());
    CHECK(!chat::validate_input({{"text", std::string(1001, 'a')}}).empty());
    CHECK(chat::validate_input({{"text", std::string(1000, 'a')}}).empty());
    CHECK(!chat::validate_input({{"text", "x"}, {"conversation_id", ""}}).empty());
    CHECK(!chat::validate_input({{"text", "x"}, {"conversation_id", "a b"}}).empty());
    CHECK(!chat::validate_input({{"text", "x"}, {"conversation_id", std::string(65, 'a')}}).empty());
    CHECK(chat::validate_input({{"text", "x"}, {"conversation_id", std::string(64, 'a')}}).empty());
    CHECK(!chat::validate_input({{"text", "x"}, {"now", {{"date", "04.11.2026"}, {"time", "14:05:00"}}}}).empty());
    CHECK(!chat::validate_input({{"text", "x"}, {"now", "сейчас"}}).empty());
}

TEST(chat_prompt_substitutions_and_real_file) {
    llm::Prompt p = llm::parse_prompt("# version: v\n{today} {weekday} {time}\n{mcp_instructions}");
    CHECK_EQ(chat::render_system_prompt(p, "2026-11-04", "14:05:00", "правила"),
             std::string("2026-11-04 среда 14:05:00\nправила"));
    CHECK_EQ(chat::render_system_prompt(p, "2026-11-04", "14:05:00", ""), std::string("2026-11-04 среда 14:05:00\n(нет)"));
    auto now = chat::prompt_now(json::object(), 1793785200);
    CHECK_EQ(now.first, std::string("2026-11-04"));
    CHECK_EQ(now.second, std::string("09:40:00 UTC"));
    auto given = chat::prompt_now({{"now", {{"date", "2026-09-30"}, {"time", "23:59:00"}}}}, 1793785200);
    CHECK_EQ(given.first, std::string("2026-09-30"));

    // Настоящий prompts/chat.txt: версия есть, подстановки на месте.
    llm::Prompt real = llm::load_prompt(std::string(WALLET_AI_SOURCE_DIR) + "/prompts/chat.txt");
    CHECK_EQ(real.version, std::string("chat-v2"));
    std::string rendered = chat::render_system_prompt(real, "2026-11-04", "14:05:00", "ИНСТРУКЦИИ MCP");
    CHECK(rendered.find("ИНСТРУКЦИИ MCP") != std::string::npos);
    CHECK(rendered.find("2026-11-04, среда") != std::string::npos);
    CHECK(rendered.find('{') == std::string::npos);
    // Без файла — исключение: main.cpp не стартует.
    CHECK_THROWS(llm::load_prompt(std::string(WALLET_AI_SOURCE_DIR) + "/prompts/no-such-chat.txt"));
}

TEST(chat_loop_nanny_error_then_baby_sitter) {
    Bench b([](int i, const std::vector<chat::Message>&, bool) {
        if (i == 0) return tool_turn("spending_by_category", {{"from", "2026-09-01"}, {"to", "2026-09-30"}, {"category", "Няня"}}, "c1");
        if (i == 1) return tool_turn("spending_by_category", {{"from", "2026-09-01"}, {"to", "2026-09-30"}, {"category", "Baby Sitter"}}, "c2");
        return text_turn("За сентябрь 2026 на няню ушло 42 000 RUB.");
    });
    b.mcp.on_call = [](const std::string&, const json& args) {
        mcp::CallResult r;
        r.status = mcp::Status::Ok;
        if (args.value("category", "") == "Няня") {
            r.is_error = true;
            r.content = "Категория «Няня» не найдена. Похожие: Baby Sitter";
        } else {
            r.content = "Baby Sitter: 42000 RUB";
        }
        return r;
    };
    auto out = chat::run(b.job(kQuestion), b.agent);
    REQUIRE(out.status == jobs::Status::Done);
    CHECK_EQ(out.result["text"].get<std::string>(), std::string("За сентябрь 2026 на няню ушло 42 000 RUB."));
    CHECK_EQ(out.result["steps"].get<int>(), 3);
    CHECK_EQ(out.result["tools"], json({"spending_by_category", "spending_by_category"}));
    CHECK_EQ(out.result["provider"].get<std::string>(), std::string("lmstudio"));
    CHECK_EQ(out.result["conversation_id"].get<std::string>(), std::string("tg-42-1"));
    CHECK_EQ(b.mcp.seen_token, std::string("mcp-token-xyz"));
    CHECK_EQ(b.mcp.inits, 1);

    // Ошибка инструмента дошла до модели результатом с is_error.
    const auto& second = b.model->calls[1];
    REQUIRE(second.size() == 3u); // вопрос, ответ с вызовом, результаты
    CHECK(second[0].role == chat::Role::User);
    CHECK_EQ(second[0].text, std::string(kQuestion));
    CHECK(second[2].role == chat::Role::Tool);
    REQUIRE(second[2].tool_results.size() == 1u);
    CHECK(second[2].tool_results[0].is_error);
    CHECK_EQ(second[2].tool_results[0].id, std::string("c1"));
    CHECK(second[2].tool_results[0].content.find("Baby Sitter") != std::string::npos);
    // Промпт: дата из input.now и instructions wallet-mcp; вопроса в нём нет.
    CHECK(b.model->systems[0].find("Сегодня 2026-11-04, среда, 14:05:00.") != std::string::npos);
    CHECK(b.model->systems[0].find("Периоды — включительно.") != std::string::npos);
    CHECK(b.model->systems[0].find(kQuestion) == std::string::npos);
    CHECK(b.model->allow[0] && b.model->allow[1] && b.model->allow[2]);
    CHECK_EQ(b.model->tool_counts[0], 3u);

    // Журнал завершения — без вопроса, ответа и сумм.
    std::string log = out.log.dump();
    CHECK_EQ(out.log["steps"].get<int>(), 3);
    CHECK_EQ(out.log["input_tokens"].get<int>(), 300);
    CHECK_EQ(out.log["output_tokens"].get<int>(), 30);
    CHECK_EQ(out.log["provider"].get<std::string>(), std::string("lmstudio"));
    CHECK(log.find("няню") == std::string::npos);
    CHECK(log.find("42") == std::string::npos);
    CHECK(log.find("Baby Sitter") == std::string::npos);
    CHECK(log.find("mcp-token") == std::string::npos);
}

TEST(chat_loop_unknown_tool_and_bad_args_not_sent) {
    Bench b([](int i, const std::vector<chat::Message>&, bool) {
        if (i == 0) {
            chat::Turn t = tool_turn("delete_operation", {{"id", "x"}}, "c1");
            t.message.tool_calls.push_back({"c2", "list_categories", json::object(), false}); // битые аргументы
            return t;
        }
        return text_turn("Удалять я не умею.");
    });
    auto out = chat::run(b.job("удали операцию"), b.agent);
    REQUIRE(out.status == jobs::Status::Done);
    CHECK(b.mcp.called.empty()); // в wallet-mcp ничего не ушло
    CHECK_EQ(out.result["tools"], json::array());
    const auto& results = b.model->calls[1].back();
    REQUIRE(results.tool_results.size() == 2u); // оба результата — одним сообщением
    CHECK(results.tool_results[0].is_error);
    CHECK(results.tool_results[0].content.find("Нет такого инструмента: delete_operation") != std::string::npos);
    CHECK(results.tool_results[1].is_error);
    CHECK_EQ(chat_metrics::value("ai_chat_tool_calls_total", {{"tool", "unknown"}, {"result", "unknown_tool"}}) > 0,
             true);
}

TEST(chat_loop_step_limit_final_call_without_tools) {
    Bench b([](int i, const std::vector<chat::Message>&, bool allow) {
        if (allow) return tool_turn("list_categories", json::object(), "c" + std::to_string(i));
        return text_turn("По полученным данным: всего 3 категории.");
    });
    auto out = chat::run(b.job("что есть?"), b.agent);
    REQUIRE(out.status == jobs::Status::Done);
    CHECK_EQ(out.result["steps"].get<int>(), 6);
    CHECK_EQ(out.result["tools"].size(), 5u);
    REQUIRE(b.model->allow.size() == 6u);
    for (int i = 0; i < 5; ++i) CHECK(b.model->allow[static_cast<std::size_t>(i)]);
    CHECK(!b.model->allow[5]);
    // Перед последним вызовом — просьба ответить по данным.
    CHECK_EQ(b.model->calls[5].back().text, std::string(chat::kFinalCallNote));
    CHECK(b.model->calls[4].back().text.empty());

    // Последний вызов снова просит инструменты без текста — internal.
    Bench stubborn([](int, const std::vector<chat::Message>&, bool) {
        return tool_turn("list_categories", json::object(), "c");
    });
    auto bad = chat::run(stubborn.job("что есть?"), stubborn.agent);
    CHECK(bad.status == jobs::Status::Failed);
    CHECK_EQ(bad.failure, std::string("internal"));
}

TEST(chat_loop_deadline_timeout) {
    Bench* self = nullptr;
    Bench b([&self](int, const std::vector<chat::Message>&, bool) {
        self->clock += 70s; // каждый вызов модели — 70 с
        return tool_turn("list_categories", json::object(), "c");
    });
    self = &b;
    auto out = chat::run(b.job("долго"), b.agent);
    CHECK(out.status == jobs::Status::Failed);
    CHECK_EQ(out.failure, std::string("timeout"));
    CHECK_EQ(b.model->calls.size(), 2u); // 70 + 70 > 120
    CHECK_EQ(b.model->timeouts[0], 120000);
    CHECK_EQ(b.model->timeouts[1], 50000); // остаток дедлайна

    Bench slow([](int, const std::vector<chat::Message>&, bool) { return error_turn(chat::ModelError::Timeout); });
    CHECK_EQ(chat::run(slow.job("x"), slow.agent).failure, std::string("timeout"));
}

TEST(chat_loop_provider_auth_failed_logs_owner) {
    Bench b([](int, const std::vector<chat::Message>&, bool) { return error_turn(chat::ModelError::AuthFailed); },
            providers::kClaude);
    parse_expense::Outcome out;
    std::string logged;
    {
        testing::CaptureOutput capture;
        out = chat::run(b.job(kQuestion), b.agent);
        logged = capture.text();
    }
    CHECK(out.status == jobs::Status::Failed);
    CHECK_EQ(out.failure, std::string("provider_auth_failed"));
    CHECK(logged.find("provider_auth_failed") != std::string::npos);
    CHECK(logged.find("\"owner\":\"instance\"") != std::string::npos);
    CHECK(logged.find("\"provider\":\"claude\"") != std::string::npos);
    CHECK(logged.find("sk-ant-secret") == std::string::npos);
    CHECK(logged.find("няню") == std::string::npos);
    // Ключ пришёл в turn() вызова, а не хранится в задании.
    REQUIRE(b.model->credentials[0].has_value());
    CHECK_EQ(b.model->credentials[0]->secret, std::string("sk-ant-secret"));
    CHECK(b.job("x").input.dump().find("sk-ant-secret") == std::string::npos);
}

TEST(chat_loop_failure_codes) {
    // wallet-mcp лежит — модель не вызывается.
    Bench down([](int, const std::vector<chat::Message>&, bool) { return text_turn("x"); });
    down.mcp.list_status = mcp::Status::ToolsUnavailable;
    auto o1 = chat::run(down.job("x"), down.agent);
    CHECK_EQ(o1.failure, std::string("tools_unavailable"));
    CHECK(down.model->calls.empty());

    Bench init_rpc([](int, const std::vector<chat::Message>&, bool) { return text_turn("x"); });
    init_rpc.mcp.init_status = mcp::Status::RpcError;
    CHECK_EQ(chat::run(init_rpc.job("x"), init_rpc.agent).failure, std::string("tools_unavailable"));

    // 401 wallet-mcp на tools/call — auth_expired.
    Bench expired([](int, const std::vector<chat::Message>&, bool) {
        return tool_turn("spending_by_category", json::object(), "c1");
    });
    expired.mcp.on_call = [](const std::string&, const json&) {
        mcp::CallResult r;
        r.status = mcp::Status::AuthExpired;
        return r;
    };
    CHECK_EQ(chat::run(expired.job("x"), expired.agent).failure, std::string("auth_expired"));

    auto refused_turn = [](int, const std::vector<chat::Message>&, bool) {
        chat::Turn t;
        t.kind = chat::TurnKind::Refused;
        t.detail = "refusal cyber";
        return t;
    };
    Bench refused(refused_turn, providers::kClaude);
    CHECK_EQ(chat::run(refused.job("x"), refused.agent).failure, std::string("provider_refused"));

    Bench cut([](int, const std::vector<chat::Message>&, bool) {
        chat::Turn t;
        t.kind = chat::TurnKind::Truncated;
        return t;
    });
    CHECK_EQ(chat::run(cut.job("x"), cut.agent).failure, std::string("internal"));

    Bench gone([](int, const std::vector<chat::Message>&, bool) { return error_turn(chat::ModelError::Unavailable); });
    CHECK_EQ(chat::run(gone.job("x"), gone.agent).failure, std::string("unavailable"));

    // Провайдера задания нет в реестре — internal без вызовов.
    Bench none([](int, const std::vector<chat::Message>&, bool) { return text_turn("x"); });
    jobs::Job j = none.job("x");
    j.input["provider"] = "gpt";
    CHECK_EQ(chat::run(j, none.agent).failure, std::string("internal"));
    CHECK_EQ(none.mcp.inits, 0);
}

TEST(chat_history_clarification_sees_previous_turns) {
    Bench b([](int i, const std::vector<chat::Message>& msgs, bool) {
        if (i == 0) return text_turn("За какой месяц?");
        // Второе задание: модель видит прежний вопрос и уточнение.
        if (msgs.size() == 3u) {
            return tool_turn("spending_by_category", {{"from", "2026-09-01"}, {"to", "2026-09-30"}}, "c1");
        }
        return text_turn("За сентябрь — 42 000 RUB.");
    });
    auto first = chat::run(b.job("сколько за няню"), b.agent);
    REQUIRE(first.status == jobs::Status::Done);
    auto second = chat::run(b.job("сентябрь"), b.agent);
    REQUIRE(second.status == jobs::Status::Done);
    const auto& msgs = b.model->calls[1];
    REQUIRE(msgs.size() == 3u);
    CHECK_EQ(msgs[0].text, std::string("сколько за няню"));
    CHECK(msgs[1].role == chat::Role::Assistant);
    CHECK_EQ(msgs[1].text, std::string("За какой месяц?"));
    CHECK(msgs[1].raw.is_null()); // между заданиями — только текст
    CHECK_EQ(msgs[2].text, std::string("сентябрь"));
    REQUIRE(b.mcp.called.size() == 1u);
    CHECK_EQ(b.mcp.called[0].second["from"].get<std::string>(), std::string("2026-09-01"));
    // Результаты инструментов в историю не попали.
    auto hist = b.conversations.get(kUser, "tg-42-1", b.clock);
    REQUIRE(hist.size() == 2u);
    CHECK_EQ(hist[1].second, std::string("За сентябрь — 42 000 RUB."));
}

TEST(chat_history_isolation_ttl_and_limit) {
    chat::Conversations c(6, 1800s);
    auto t0 = std::chrono::steady_clock::time_point{} + 1000h;
    c.append(kUser, "conv-1", "q", "a", t0);
    CHECK(c.get(kOther, "conv-1", t0).empty()); // чужой диалог — пусто
    CHECK_EQ(c.get(kUser, "conv-1", t0 + 1799s).size(), 1u);
    // TTL — от последнего обращения: get продлил.
    CHECK_EQ(c.get(kUser, "conv-1", t0 + 1799s + 1799s).size(), 1u);
    CHECK(c.get(kUser, "conv-1", t0 + 1799s + 1799s + 1800s).empty());
    CHECK_EQ(c.size(), 0u);

    for (int i = 0; i < 8; ++i) c.append(kUser, "conv-2", "q" + std::to_string(i), "a", t0);
    auto turns = c.get(kUser, "conv-2", t0);
    REQUIRE(turns.size() == 6u);
    CHECK_EQ(turns.front().first, std::string("q2"));
    c.append(kOther, "conv-3", "q", "a", t0);
    c.sweep(t0 + 1800s);
    CHECK_EQ(c.size(), 0u);

    // В задании чужой пользователь с тем же conversation_id историю не видит.
    Bench b([](int, const std::vector<chat::Message>&, bool) { return text_turn("ответ"); });
    chat::run(b.job("вопрос A"), b.agent);
    chat::run(b.job("вопрос B", "tg-42-1", kOther), b.agent);
    CHECK_EQ(b.model->calls[1].size(), 1u);
}

TEST(chat_metrics_tokens_jobs_steps) {
    auto before_in = chat_metrics::value("ai_chat_tokens_total", {{"provider", "claude"}, {"direction", "input"}});
    auto before_out = chat_metrics::value("ai_chat_tokens_total", {{"provider", "claude"}, {"direction", "output"}});
    auto before_done = chat_metrics::value("ai_chat_jobs_total", {{"provider", "claude"}, {"result", "done"}});
    Bench b(
        [](int i, const std::vector<chat::Message>&, bool) {
            if (i < 2) return tool_turn("list_categories", json::object(), "c", 1000, 50);
            return text_turn("готово", 1200, 70);
        },
        providers::kClaude);
    auto out = chat::run(b.job("x"), b.agent);
    REQUIRE(out.status == jobs::Status::Done);
    CHECK_EQ(chat_metrics::value("ai_chat_tokens_total", {{"provider", "claude"}, {"direction", "input"}}) - before_in,
             3200u);
    CHECK_EQ(chat_metrics::value("ai_chat_tokens_total", {{"provider", "claude"}, {"direction", "output"}}) - before_out,
             170u);
    CHECK_EQ(chat_metrics::value("ai_chat_jobs_total", {{"provider", "claude"}, {"result", "done"}}) - before_done, 1u);
    std::string text = chat_metrics::render();
    CHECK(text.find("ai_chat_tokens_total{provider=\"claude\",direction=\"input\"}") != std::string::npos);
    CHECK(text.find("ai_chat_steps_bucket{le=\"3\"}") != std::string::npos);
    CHECK(text.find("ai_chat_tool_calls_total{tool=\"list_categories\",result=\"ok\"}") != std::string::npos);
}
