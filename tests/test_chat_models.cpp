// Адаптеры моделей чата (add-chat-agent, tasks 3.2/3.3): тела запросов и
// разбор ответов — на записанных образцах. LM Studio — формат OpenAI
// chat/completions, как его отдаёт LM Studio 0.3.x с gemma-4-e4b; Anthropic —
// Messages API версии 2026-10-21.
#include "chat_models.hpp"

#include "fake_http.hpp"
#include "test_framework.hpp"

using nlohmann::json;

namespace {

std::vector<mcp::Tool> sample_tools() {
    mcp::Tool t;
    t.name = "spending_by_category";
    t.description = "Траты по категориям за период";
    t.input_schema = {{"type", "object"},
                      {"properties", {{"from", {{"type", "string"}}}, {"to", {{"type", "string"}}}}},
                      {"required", {"from", "to"}}};
    return {t};
}

// LM Studio: модель просит инструмент (finish_reason tool_calls).
const char* kLmToolCall = R"({
  "id": "chatcmpl-8f2k1",
  "object": "chat.completion",
  "created": 1793785200,
  "model": "gemma-4-e4b",
  "choices": [{
    "index": 0,
    "logprobs": null,
    "finish_reason": "tool_calls",
    "message": {
      "role": "assistant",
      "content": "",
      "reasoning_content": "Нужны траты за сентябрь.",
      "tool_calls": [{
        "id": "512837406",
        "type": "function",
        "function": {"name": "spending_by_category", "arguments": "{\"from\":\"2026-09-01\",\"to\":\"2026-09-30\",\"category\":\"Няня\"}"}
      }]
    }
  }],
  "usage": {"prompt_tokens": 2310, "completion_tokens": 48, "total_tokens": 2358},
  "stats": {},
  "system_fingerprint": "gemma-4-e4b"
})";

// LM Studio: текст ответа с остатком <think>.
const char* kLmText = R"({
  "id": "chatcmpl-8f2k2",
  "object": "chat.completion",
  "model": "gemma-4-e4b",
  "choices": [{
    "index": 0,
    "finish_reason": "stop",
    "message": {"role": "assistant", "content": "<think>сумма 42 000</think>\n\nЗа сентябрь 2026 на няню ушло 42 000 RUB."}
  }],
  "usage": {"prompt_tokens": 2602, "completion_tokens": 31, "total_tokens": 2633}
})";

// LM Studio: оборвано по max_tokens.
const char* kLmLength = R"({
  "id": "chatcmpl-8f2k3",
  "object": "chat.completion",
  "model": "gemma-4-e4b",
  "choices": [{"index": 0, "finish_reason": "length", "message": {"role": "assistant", "content": "За сентябрь на ня"}}],
  "usage": {"prompt_tokens": 2602, "completion_tokens": 1024, "total_tokens": 3626}
})";

// LM Studio: аргументы — битый JSON, у второго вызова нет id.
const char* kLmBrokenArgs = R"({
  "choices": [{
    "index": 0,
    "finish_reason": "tool_calls",
    "message": {
      "role": "assistant",
      "content": null,
      "tool_calls": [
        {"id": "1", "type": "function", "function": {"name": "spending_by_category", "arguments": "{from: 2026-09-01"}},
        {"type": "function", "function": {"name": "list_categories", "arguments": ""}}
      ]
    }
  }],
  "usage": {"prompt_tokens": 100, "completion_tokens": 20}
})";

// Anthropic: thinking (пустой — display omitted), текст, tool_use.
const char* kClaudeToolUse = R"({
  "id": "msg_01XFDUDYJgAACzvnptvVoYEL",
  "type": "message",
  "role": "assistant",
  "model": "claude-opus-5-5",
  "content": [
    {"type": "thinking", "thinking": "", "signature": "EqQBCkYIBhgCIkB..."},
    {"type": "text", "text": "Посмотрю траты за сентябрь."},
    {"type": "tool_use", "id": "toolu_01A09q90qw90lq917835lq9", "name": "spending_by_category",
     "input": {"from": "2026-09-01", "to": "2026-09-30"}},
    {"type": "tool_use", "id": "toolu_01B", "name": "list_categories", "input": {}}
  ],
  "stop_reason": "tool_use",
  "stop_sequence": null,
  "stop_details": null,
  "usage": {"input_tokens": 3120, "output_tokens": 210, "cache_read_input_tokens": 0}
})";

const char* kClaudeText = R"({
  "id": "msg_02", "type": "message", "role": "assistant", "model": "claude-opus-5-5",
  "content": [
    {"type": "thinking", "thinking": "", "signature": "Eq..."},
    {"type": "text", "text": "За сентябрь 2026 на няню — 42 000 RUB."}
  ],
  "stop_reason": "end_turn", "stop_details": null,
  "usage": {"input_tokens": 3500, "output_tokens": 95}
})";

const char* kClaudeRefusal = R"({
  "id": "msg_03", "type": "message", "role": "assistant", "model": "claude-opus-5-5",
  "content": [{"type": "text", "text": "частичный текст"}],
  "stop_reason": "refusal",
  "stop_details": {"type": "refusal", "category": "cyber", "explanation": "..."},
  "usage": {"input_tokens": 1200, "output_tokens": 3}
})";

const char* kClaudeMaxTokens = R"({
  "id": "msg_04", "type": "message", "role": "assistant", "model": "claude-opus-5-5",
  "content": [{"type": "thinking", "thinking": "", "signature": "x"}],
  "stop_reason": "max_tokens", "stop_details": null,
  "usage": {"input_tokens": 1200, "output_tokens": 16000}
})";

} // namespace

// --- OpenAI / LM Studio ---

TEST(openai_parse_tool_call) {
    chat::Turn t = chat::parse_openai_response(200, kLmToolCall);
    REQUIRE(t.kind == chat::TurnKind::ToolCalls);
    REQUIRE(t.message.tool_calls.size() == 1u);
    const auto& c = t.message.tool_calls[0];
    CHECK_EQ(c.id, std::string("512837406"));
    CHECK_EQ(c.name, std::string("spending_by_category"));
    CHECK(c.args_ok);
    CHECK_EQ(c.args["from"].get<std::string>(), std::string("2026-09-01"));
    CHECK_EQ(t.usage.input_tokens, 2310);
    CHECK_EQ(t.usage.output_tokens, 48);
    CHECK(!t.message.raw.contains("reasoning_content")); // рассуждения обратно не шлём
}

TEST(openai_parse_text_strips_think) {
    chat::Turn t = chat::parse_openai_response(200, kLmText);
    REQUIRE(t.kind == chat::TurnKind::Text);
    CHECK_EQ(t.message.text, std::string("За сентябрь 2026 на няню ушло 42 000 RUB."));
    CHECK_EQ(t.usage.output_tokens, 31);
}

TEST(openai_parse_length_and_errors) {
    CHECK(chat::parse_openai_response(200, kLmLength).kind == chat::TurnKind::Truncated);
    auto auth = chat::parse_openai_response(401, "");
    CHECK(auth.kind == chat::TurnKind::Error);
    CHECK(auth.error == chat::ModelError::AuthFailed);
    CHECK(chat::parse_openai_response(503, "").error == chat::ModelError::Unavailable);
    CHECK(chat::parse_openai_response(404, "").error == chat::ModelError::Unavailable);
    CHECK(chat::parse_openai_response(400, "").error == chat::ModelError::BadResponse);
    CHECK(chat::parse_openai_response(200, "not json").error == chat::ModelError::BadResponse);
    CHECK(chat::parse_openai_response(200, R"({"choices":[{"finish_reason":"stop","message":{"content":"<think>x</think>"}}]})")
              .error == chat::ModelError::BadResponse); // пустой ответ
}

TEST(openai_parse_broken_args_and_missing_id) {
    chat::Turn t = chat::parse_openai_response(200, kLmBrokenArgs);
    REQUIRE(t.kind == chat::TurnKind::ToolCalls);
    REQUIRE(t.message.tool_calls.size() == 2u);
    CHECK(!t.message.tool_calls[0].args_ok);          // не JSON — ошибка инструмента модели
    CHECK(t.message.tool_calls[1].args_ok);           // пустая строка — пустой объект
    CHECK_EQ(t.message.tool_calls[1].id, std::string("call_2"));
    CHECK_EQ(t.message.raw["tool_calls"][1]["id"].get<std::string>(), std::string("call_2"));
}

TEST(openai_body_tools_and_tool_role) {
    chat::OpenAiConfig cfg;
    cfg.model = "gemma-4-e4b";
    chat::Turn first = chat::parse_openai_response(200, kLmToolCall);
    std::vector<chat::Message> msgs;
    msgs.push_back({chat::Role::User, "прошлый вопрос", {}, {}, nullptr});
    msgs.push_back({chat::Role::Assistant, "прошлый ответ", {}, {}, nullptr});
    msgs.push_back({chat::Role::User, "сколько за няню в сентябре", {}, {}, nullptr});
    msgs.push_back(first.message);
    chat::Message results;
    results.role = chat::Role::Tool;
    results.tool_results.push_back({"512837406", "Категория «Няня» не найдена. Похожие: Baby Sitter", true});
    msgs.push_back(results);

    json body = chat::openai_request_body(cfg, "SYSTEM", msgs, sample_tools(), true);
    CHECK_EQ(body["model"].get<std::string>(), std::string("gemma-4-e4b"));
    CHECK_EQ(body["max_tokens"].get<int>(), 1024);
    CHECK_EQ(body["reasoning_effort"].get<std::string>(), std::string("none"));
    CHECK(!body.contains("temperature"));
    const auto& m = body["messages"];
    REQUIRE(m.size() == 6u);
    CHECK_EQ(m[0]["role"].get<std::string>(), std::string("system"));
    CHECK_EQ(m[3]["content"].get<std::string>(), std::string("сколько за няню в сентябре"));
    CHECK_EQ(m[4]["role"].get<std::string>(), std::string("assistant"));
    CHECK(m[4]["content"].is_null());
    CHECK_EQ(m[4]["tool_calls"][0]["function"]["arguments"].get<std::string>(),
             std::string("{\"from\":\"2026-09-01\",\"to\":\"2026-09-30\",\"category\":\"Няня\"}"));
    CHECK_EQ(m[5]["role"].get<std::string>(), std::string("tool"));
    CHECK_EQ(m[5]["tool_call_id"].get<std::string>(), std::string("512837406"));
    CHECK(m[5]["content"].get<std::string>().rfind(chat::kOpenAiErrorPrefix, 0) == 0);
    REQUIRE(body["tools"].size() == 1u);
    CHECK_EQ(body["tools"][0]["type"].get<std::string>(), std::string("function"));
    CHECK_EQ(body["tools"][0]["function"]["parameters"]["required"][0].get<std::string>(), std::string("from"));

    // Последний вызов: без tools, приписка — отдельным сообщением user.
    msgs.back().text = "ответь без инструментов";
    json last = chat::openai_request_body(cfg, "SYSTEM", msgs, sample_tools(), false);
    CHECK(!last.contains("tools"));
    CHECK_EQ(last["messages"].back()["role"].get<std::string>(), std::string("user"));
}

TEST(openai_http_sends_bearer_from_credential) {
    testing::FakeHttpServer srv("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kLmText, "application/json");
    });
    chat::OpenAiConfig cfg;
    cfg.url = srv.origin() + "/v1";
    cfg.model = "m";
    auto model = chat::make_openai_model(cfg);
    std::vector<chat::Message> msgs = {{chat::Role::User, "q", {}, {}, nullptr}};
    chat::Turn t = model->turn("s", msgs, sample_tools(), true, chat::Credential{"lm-key", chat::Owner::Instance}, 5000);
    CHECK(t.kind == chat::TurnKind::Text);
    auto reqs = srv.requests();
    REQUIRE(reqs.size() == 1u);
    CHECK_EQ(reqs[0].get_header_value("Authorization"), std::string("Bearer lm-key"));

    chat::OpenAiConfig off;
    CHECK(chat::make_openai_model(off)->turn("s", msgs, {}, true, std::nullopt, 1000).error ==
          chat::ModelError::Unavailable);
}

// --- Anthropic ---

TEST(anthropic_body_shape) {
    chat::AnthropicConfig cfg;
    std::vector<chat::Message> msgs = {{chat::Role::User, "сколько за няню", {}, {}, nullptr}};
    json body = chat::anthropic_request_body(cfg, "SYSTEM", msgs, sample_tools(), true);
    CHECK_EQ(body["model"].get<std::string>(), std::string("claude-opus-5-5"));
    CHECK_EQ(body["max_tokens"].get<int>(), 16000);
    CHECK_EQ(body["system"].get<std::string>(), std::string("SYSTEM"));
    CHECK_EQ(body["thinking"], json({{"type", "adaptive"}}));
    CHECK_EQ(body["output_config"], json({{"effort", "low"}}));
    CHECK_EQ(body["tool_choice"], json({{"type", "auto"}}));
    CHECK_EQ(body["fallbacks"].get<std::string>(), std::string("default"));
    CHECK_EQ(body["tools"][0]["name"].get<std::string>(), std::string("spending_by_category"));
    CHECK(body["tools"][0].contains("input_schema"));
    CHECK(!body["tools"][0].contains("type"));
    for (const char* k : {"temperature", "top_p", "top_k", "stream"}) CHECK(!body.contains(k));
    CHECK_EQ(body["messages"].back()["role"].get<std::string>(), std::string("user")); // без prefill

    cfg.fallbacks = false;
    cfg.effort = "medium";
    json off = chat::anthropic_request_body(cfg, "SYSTEM", msgs, sample_tools(), false);
    CHECK(!off.contains("fallbacks"));
    CHECK_EQ(off["tool_choice"], json({{"type", "none"}})); // последний вызов — без инструментов
    CHECK_EQ(off["output_config"]["effort"].get<std::string>(), std::string("medium"));
}

TEST(anthropic_headers_version_and_beta) {
    chat::AnthropicConfig cfg;
    auto h = chat::anthropic_headers(cfg, {"sk-ant-test", chat::Owner::Instance});
    auto get = [&](const std::string& k) {
        for (const auto& [name, v] : h) {
            if (name == k) return v;
        }
        return std::string();
    };
    CHECK_EQ(get("x-api-key"), std::string("sk-ant-test"));
    CHECK_EQ(get("anthropic-version"), std::string("2026-10-21"));
    CHECK_EQ(get("anthropic-beta"), std::string("server-side-fallback-2026-07-01"));
    CHECK_EQ(get("content-type"), std::string("application/json"));
    cfg.fallbacks = false;
    for (const auto& [name, v] : chat::anthropic_headers(cfg, {"k", chat::Owner::Instance})) {
        CHECK(name != "anthropic-beta");
    }
}

TEST(anthropic_append_only_history_and_single_tool_result_message) {
    chat::AnthropicConfig cfg;
    chat::Turn t = chat::parse_anthropic_response(200, kClaudeToolUse);
    REQUIRE(t.kind == chat::TurnKind::ToolCalls);
    REQUIRE(t.message.tool_calls.size() == 2u);
    CHECK_EQ(t.message.tool_calls[0].id, std::string("toolu_01A09q90qw90lq917835lq9"));
    CHECK_EQ(t.message.tool_calls[0].args["to"].get<std::string>(), std::string("2026-09-30"));
    CHECK_EQ(t.message.text, std::string("Посмотрю траты за сентябрь."));
    CHECK_EQ(t.usage.input_tokens, 3120);
    CHECK_EQ(t.usage.output_tokens, 210);

    std::vector<chat::Message> msgs;
    msgs.push_back({chat::Role::User, "вопрос из истории", {}, {}, nullptr});
    msgs.push_back({chat::Role::Assistant, "ответ из истории", {}, {}, nullptr});
    msgs.push_back({chat::Role::User, "сколько за няню", {}, {}, nullptr});
    msgs.push_back(t.message);
    chat::Message results;
    results.role = chat::Role::Tool;
    results.tool_results.push_back({"toolu_01A09q90qw90lq917835lq9", "Няня: 42000 RUB", false});
    results.tool_results.push_back({"toolu_01B", "нет доступа", true});
    results.text = "ответь по данным";
    msgs.push_back(results);

    json body = chat::anthropic_request_body(cfg, "S", msgs, sample_tools(), true);
    const auto& m = body["messages"];
    REQUIRE(m.size() == 5u);
    CHECK_EQ(m[1]["content"].get<std::string>(), std::string("ответ из истории")); // история — только текст
    // Ответ ассистента — content целиком, вместе с пустым thinking и подписью.
    json original = json::parse(kClaudeToolUse)["content"];
    CHECK_EQ(m[3]["content"], original);
    CHECK_EQ(m[3]["content"][0]["type"].get<std::string>(), std::string("thinking"));
    // Все tool_result хода — одним сообщением user, приписка — после них.
    CHECK_EQ(m[4]["role"].get<std::string>(), std::string("user"));
    REQUIRE(m[4]["content"].size() == 3u);
    CHECK_EQ(m[4]["content"][0]["type"].get<std::string>(), std::string("tool_result"));
    CHECK_EQ(m[4]["content"][0]["tool_use_id"].get<std::string>(), std::string("toolu_01A09q90qw90lq917835lq9"));
    CHECK_EQ(m[4]["content"][0]["is_error"].get<bool>(), false);
    CHECK_EQ(m[4]["content"][1]["is_error"].get<bool>(), true);
    CHECK_EQ(m[4]["content"][2]["type"].get<std::string>(), std::string("text"));
}

TEST(anthropic_parse_text_refusal_max_tokens) {
    chat::Turn text = chat::parse_anthropic_response(200, kClaudeText);
    REQUIRE(text.kind == chat::TurnKind::Text);
    CHECK_EQ(text.message.text, std::string("За сентябрь 2026 на няню — 42 000 RUB."));
    CHECK_EQ(text.usage.output_tokens, 95);

    chat::Turn refused = chat::parse_anthropic_response(200, kClaudeRefusal);
    CHECK(refused.kind == chat::TurnKind::Refused);
    CHECK_EQ(refused.detail, std::string("refusal cyber"));
    CHECK(refused.message.text.empty()); // content при отказе не читаем

    chat::Turn cut = chat::parse_anthropic_response(200, kClaudeMaxTokens);
    CHECK(cut.kind == chat::TurnKind::Truncated);
    CHECK_EQ(cut.usage.output_tokens, 16000);

    chat::Turn paused = chat::parse_anthropic_response(
        200, R"({"content":[],"stop_reason":"pause_turn","usage":{"input_tokens":1,"output_tokens":1}})");
    CHECK(paused.error == chat::ModelError::BadResponse);
}

TEST(anthropic_parse_http_errors) {
    CHECK(chat::parse_anthropic_response(401, R"({"type":"error","error":{"type":"authentication_error"}})").error ==
          chat::ModelError::AuthFailed);
    CHECK(chat::parse_anthropic_response(403, "").error == chat::ModelError::AuthFailed);
    CHECK(chat::parse_anthropic_response(429, "").error == chat::ModelError::Unavailable);
    CHECK(chat::parse_anthropic_response(529, "").error == chat::ModelError::Unavailable);
    CHECK(chat::parse_anthropic_response(500, "").error == chat::ModelError::Unavailable);
    auto bad = chat::parse_anthropic_response(
        400, R"({"type":"error","error":{"type":"invalid_request_error","message":"секретный текст"}})");
    CHECK(bad.error == chat::ModelError::BadResponse);
    CHECK_EQ(bad.detail, std::string("http_400 invalid_request_error")); // message в журнал не идёт
}

TEST(anthropic_http_headers_and_auth_failure) {
    int calls = 0;
    testing::FakeHttpServer srv("/v1/messages", [&](const httplib::Request& req, httplib::Response& res) {
        ++calls;
        if (req.get_header_value("x-api-key") != "good") {
            res.status = 401;
            res.set_content(R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})",
                            "application/json");
            return;
        }
        res.set_content(kClaudeText, "application/json");
    });
    chat::AnthropicConfig cfg;
    cfg.base_url = srv.origin();
    auto model = chat::make_anthropic_model(cfg);
    std::vector<chat::Message> msgs = {{chat::Role::User, "q", {}, {}, nullptr}};
    chat::Turn ok = model->turn("s", msgs, sample_tools(), true, chat::Credential{"good", chat::Owner::Instance}, 5000);
    CHECK(ok.kind == chat::TurnKind::Text);
    auto reqs = srv.requests();
    REQUIRE(reqs.size() == 1u);
    CHECK_EQ(reqs[0].get_header_value("anthropic-version"), std::string("2026-10-21"));
    CHECK_EQ(reqs[0].get_header_value("anthropic-beta"), std::string("server-side-fallback-2026-07-01"));
    CHECK(reqs[0].get_header_value("Content-Type").rfind("application/json", 0) == 0);
    json sent = json::parse(reqs[0].body);
    CHECK(!sent.contains("temperature"));

    chat::Turn denied = model->turn("s", msgs, sample_tools(), true, chat::Credential{"bad", chat::Owner::Instance}, 5000);
    CHECK(denied.error == chat::ModelError::AuthFailed);
    // Без ключа — в сеть не идём.
    CHECK(model->turn("s", msgs, {}, true, std::nullopt, 5000).error == chat::ModelError::Unavailable);
    CHECK_EQ(calls, 2);
}
