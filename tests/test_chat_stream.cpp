// Потоковая генерация chat (change chat-sse-events, tasks 2.1–2.4): разбор SSE,
// фильтр <think>, сборщики OpenAI/Anthropic и turn_stream на локальном сервере.
#include <atomic>
#include <string>
#include <vector>

#include "chat_models.hpp"
#include "chat_stream.hpp"
#include "fake_http.hpp"
#include "test_framework.hpp"

using nlohmann::json;

namespace {

std::string sse(const std::vector<std::string>& data) {
    std::string out;
    for (const auto& d : data) out += "data: " + d + "\n\n";
    return out;
}

std::string oa_chunk(const json& delta, const std::string& finish = "") {
    json c = {{"choices", json::array({{{"delta", delta}, {"finish_reason", finish.empty() ? json() : json(finish)}}})}};
    return c.dump();
}

struct Collected {
    std::vector<std::string> deltas;
    std::string joined() const {
        std::string s;
        for (const auto& d : deltas) s += d;
        return s;
    }
};

chat::StreamSink sink_into(Collected& c, std::function<bool()> stop = nullptr) {
    chat::StreamSink s;
    s.on_delta = [&c](const std::string& t) { c.deltas.push_back(t); };
    s.should_stop = std::move(stop);
    return s;
}

std::vector<chat::Message> question() {
    chat::Message m;
    m.role = chat::Role::User;
    m.text = "сколько за няню";
    return {m};
}

chat::OpenAiConfig oa_cfg(const testing::FakeHttpServer& srv) {
    chat::OpenAiConfig cfg;
    cfg.url = srv.origin() + "/v1";
    cfg.model = "m";
    return cfg;
}

void reply_sse(httplib::Response& res, const std::string& body) { res.set_content(body, "text/event-stream"); }

} // namespace

TEST(sse_parser_splits_events_across_feeds) {
    chat::SseParser p;
    std::vector<std::string> got;
    auto on = [&](const std::string& d) { got.push_back(d); };
    std::string raw = ": ping\r\ndata: one\r\n\r\nevent: x\ndata: two\ndata: three\n\ndata: tail";
    for (char c : raw) p.feed(&c, 1, on); // по байту: границы кадров не важны
    REQUIRE(got.size() == 2u);
    CHECK_EQ(got[0], std::string("one"));
    CHECK_EQ(got[1], std::string("two\nthree"));
}

TEST(think_filter_matches_strip_think) {
    auto run = [](const std::vector<std::string>& chunks) {
        chat::ThinkFilter f;
        std::string out;
        for (const auto& c : chunks) out += f.push(c);
        return out;
    };
    CHECK_EQ(run({"  Привет", ", мир \n"}), std::string("Привет, мир"));
    CHECK_EQ(run({"<thi", "nk>раз", "мышляю</th", "ink>\n\nОтвет ", "готов"}), std::string("Ответ готов"));
    CHECK_EQ(run({"<think>x</think>"}), std::string(""));
    CHECK_EQ(run({"<b>жирный</b>"}), std::string("<b>жирный</b>")); // не think — как есть
}

TEST(openai_stream_text_and_usage) {
    testing::FakeHttpServer srv("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        reply_sse(res, sse({oa_chunk({{"role", "assistant"}, {"content", ""}}), oa_chunk({{"content", "На няню "}}),
                            oa_chunk({{"content", "ушло 5000."}}), oa_chunk(json::object(), "stop"),
                            R"({"choices":[],"usage":{"prompt_tokens":40,"completion_tokens":7}})", "[DONE]"}));
    });
    Collected got;
    auto t = chat::make_openai_model(oa_cfg(srv))->turn_stream("s", question(), {}, true, std::nullopt, 5000, sink_into(got));
    CHECK(t.kind == chat::TurnKind::Text);
    CHECK_EQ(t.message.text, std::string("На няню ушло 5000."));
    CHECK_EQ(got.joined(), t.message.text); // склейка delta = итог
    CHECK(got.deltas.size() >= 2u);
    CHECK_EQ(t.usage.input_tokens, 40);
    CHECK_EQ(t.usage.output_tokens, 7);
    auto body = json::parse(srv.requests().at(0).body);
    CHECK(body["stream"].get<bool>());
    CHECK(body["stream_options"]["include_usage"].get<bool>());
}

TEST(openai_stream_hides_think_block) {
    testing::FakeHttpServer srv("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        reply_sse(res, sse({oa_chunk({{"content", "<think>надо посчитать"}}), oa_chunk({{"content", "</think>\n\nОтвет"}}),
                            oa_chunk({{"content", " 42"}}), oa_chunk(json::object(), "stop"), "[DONE]"}));
    });
    Collected got;
    auto t = chat::make_openai_model(oa_cfg(srv))->turn_stream("s", question(), {}, true, std::nullopt, 5000, sink_into(got));
    CHECK(t.kind == chat::TurnKind::Text);
    CHECK_EQ(t.message.text, std::string("Ответ 42"));
    CHECK_EQ(got.joined(), std::string("Ответ 42"));
}

TEST(openai_stream_tool_call_assembled_from_parts) {
    testing::FakeHttpServer srv("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        auto call = [](const json& fn, bool first) {
            json tc = {{"index", 0}, {"function", fn}};
            if (first) {
                tc["id"] = "call_9";
                tc["type"] = "function";
            }
            return oa_chunk({{"tool_calls", json::array({tc})}});
        };
        reply_sse(res, sse({call({{"name", "spending_by_category"}, {"arguments", ""}}, true),
                            call({{"arguments", "{\"from\":\"2026-11-"}}, false), call({{"arguments", "01\"}"}}, false),
                            oa_chunk(json::object(), "tool_calls"), "[DONE]"}));
    });
    Collected got;
    auto t = chat::make_openai_model(oa_cfg(srv))->turn_stream("s", question(), {}, true, std::nullopt, 5000, sink_into(got));
    CHECK(t.kind == chat::TurnKind::ToolCalls);
    REQUIRE(t.message.tool_calls.size() == 1u); // вызов один, с целыми аргументами
    CHECK_EQ(t.message.tool_calls[0].id, std::string("call_9"));
    CHECK_EQ(t.message.tool_calls[0].name, std::string("spending_by_category"));
    CHECK_EQ(t.message.tool_calls[0].args["from"].get<std::string>(), std::string("2026-11-01"));
    CHECK(t.message.tool_calls[0].args_ok);
    CHECK(got.deltas.empty());
}

TEST(openai_stream_cut_midway_is_unavailable) {
    testing::FakeHttpServer srv("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        reply_sse(res, sse({oa_chunk({{"content", "Начало отве"}})})); // ни finish_reason, ни [DONE]
    });
    Collected got;
    auto t = chat::make_openai_model(oa_cfg(srv))->turn_stream("s", question(), {}, true, std::nullopt, 5000, sink_into(got));
    CHECK(t.kind == chat::TurnKind::Error);
    CHECK(t.error == chat::ModelError::Unavailable);
    CHECK_EQ(t.detail, std::string("stream_cut"));
    CHECK_EQ(got.joined(), std::string("Начало отве")); // уже показанное не отзывается, но итогом не становится
}

TEST(openai_stream_stop_requested_aborts) {
    testing::FakeHttpServer srv("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        reply_sse(res, sse({oa_chunk({{"content", "текст"}}), oa_chunk(json::object(), "stop"), "[DONE]"}));
    });
    Collected got;
    auto t = chat::make_openai_model(oa_cfg(srv))
                 ->turn_stream("s", question(), {}, true, std::nullopt, 5000, sink_into(got, [] { return true; }));
    CHECK(t.kind == chat::TurnKind::Error);
    CHECK_EQ(t.detail, std::string("cancelled"));
    CHECK(got.deltas.empty());
}

TEST(openai_stream_errors_and_plain_json_fallback) {
    std::atomic<int> mode{0};
    testing::FakeHttpServer srv("/v1/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
        if (mode == 0) {
            res.status = 401;
            res.set_content(R"({"error":{"message":"bad key"}})", "application/json");
        } else {
            // Сервер проигнорировал stream и ответил обычным JSON.
            res.set_content(R"({"choices":[{"finish_reason":"stop","message":{"content":"Целиком"}}]})",
                            "application/json");
        }
    });
    auto model = chat::make_openai_model(oa_cfg(srv));
    Collected a;
    auto denied = model->turn_stream("s", question(), {}, true, std::nullopt, 5000, sink_into(a));
    CHECK(denied.error == chat::ModelError::AuthFailed);
    mode = 1;
    Collected b;
    auto whole = model->turn_stream("s", question(), {}, true, std::nullopt, 5000, sink_into(b));
    CHECK(whole.kind == chat::TurnKind::Text);
    CHECK_EQ(b.joined(), std::string("Целиком"));
}

TEST(anthropic_stream_text_tool_and_thinking) {
    auto ev = [](const json& j) { return j.dump(); };
    testing::FakeHttpServer srv("/v1/messages", [&](const httplib::Request&, httplib::Response& res) {
        reply_sse(res, sse({
            ev({{"type", "message_start"}, {"message", {{"usage", {{"input_tokens", 25}, {"output_tokens", 1}}}}}}),
            ev({{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "thinking"}, {"thinking", ""}}}}),
            ev({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "thinking_delta"}, {"thinking", "хм"}}}}),
            ev({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "signature_delta"}, {"signature", "sig1"}}}}),
            ev({{"type", "content_block_stop"}, {"index", 0}}),
            ev({{"type", "content_block_start"}, {"index", 1}, {"content_block", {{"type", "text"}, {"text", ""}}}}),
            ev({{"type", "content_block_delta"}, {"index", 1}, {"delta", {{"type", "text_delta"}, {"text", "Смотрю "}}}}),
            ev({{"type", "content_block_delta"}, {"index", 1}, {"delta", {{"type", "text_delta"}, {"text", "данные."}}}}),
            ev({{"type", "content_block_stop"}, {"index", 1}}),
            ev({{"type", "content_block_start"}, {"index", 2},
                {"content_block", {{"type", "tool_use"}, {"id", "tu_1"}, {"name", "balance"}, {"input", json::object()}}}}),
            ev({{"type", "content_block_delta"}, {"index", 2}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"wal"}}}}),
            ev({{"type", "content_block_delta"}, {"index", 2}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "let\":\"Cash\"}"}}}}),
            ev({{"type", "content_block_stop"}, {"index", 2}}),
            ev({{"type", "message_delta"}, {"delta", {{"stop_reason", "tool_use"}}}, {"usage", {{"output_tokens", 33}}}}),
            ev({{"type", "message_stop"}}),
        }));
    });
    chat::AnthropicConfig cfg;
    cfg.base_url = srv.origin();
    chat::Credential cred;
    cred.secret = "k";
    Collected got;
    auto t = chat::make_anthropic_model(cfg)->turn_stream("s", question(), {}, true, cred, 5000, sink_into(got));
    CHECK(t.kind == chat::TurnKind::ToolCalls);
    CHECK_EQ(got.joined(), std::string("Смотрю данные.")); // thinking в поток не попадает
    CHECK_EQ(t.message.text, std::string("Смотрю данные."));
    REQUIRE(t.message.tool_calls.size() == 1u);
    CHECK_EQ(t.message.tool_calls[0].args["wallet"].get<std::string>(), std::string("Cash"));
    CHECK_EQ(t.usage.input_tokens, 25);
    CHECK_EQ(t.usage.output_tokens, 33);
    // raw — content целиком, с thinking и подписью (append-only для следующего хода).
    REQUIRE(t.message.raw.is_array() && t.message.raw.size() == 3u);
    CHECK_EQ(t.message.raw[0]["signature"].get<std::string>(), std::string("sig1"));
    CHECK(srv.requests().at(0).body.find("\"stream\":true") != std::string::npos);
}

TEST(anthropic_stream_error_event_is_unavailable) {
    testing::FakeHttpServer srv("/v1/messages", [](const httplib::Request&, httplib::Response& res) {
        reply_sse(res, sse({R"({"type":"message_start","message":{"usage":{"input_tokens":1}}})",
                            R"({"type":"error","error":{"type":"overloaded_error","message":"x"}})"}));
    });
    chat::AnthropicConfig cfg;
    cfg.base_url = srv.origin();
    chat::Credential cred;
    cred.secret = "k";
    Collected got;
    auto t = chat::make_anthropic_model(cfg)->turn_stream("s", question(), {}, true, cred, 5000, sink_into(got));
    CHECK(t.kind == chat::TurnKind::Error);
    CHECK(t.error == chat::ModelError::Unavailable);
}
