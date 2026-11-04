#include "chat_models.hpp"

#include "httplib.h"
#include "llm_client.hpp"
#include "llm_probe.hpp"

using nlohmann::json;

namespace chat {

const char* to_string(Owner o) { return o == Owner::User ? "user" : "instance"; }

const char* to_string(TurnKind k) {
    switch (k) {
    case TurnKind::Text: return "text";
    case TurnKind::ToolCalls: return "tool_calls";
    case TurnKind::Refused: return "refused";
    case TurnKind::Truncated: return "truncated";
    case TurnKind::Error: return "error";
    }
    return "error";
}

const char* to_string(ModelError e) {
    switch (e) {
    case ModelError::None: return "none";
    case ModelError::AuthFailed: return "auth_failed";
    case ModelError::Unavailable: return "unavailable";
    case ModelError::Timeout: return "timeout";
    case ModelError::BadResponse: return "bad_response";
    }
    return "bad_response";
}

namespace {

Turn error_turn(ModelError e, std::string detail) {
    Turn t;
    t.kind = TurnKind::Error;
    t.error = e;
    t.detail = std::move(detail);
    return t;
}

bool is_str(const json& j, const char* key) { return j.is_object() && j.contains(key) && j[key].is_string(); }

int int_field(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number_integer()) return 0;
    return j[key].get<int>();
}

// Сетевая ошибка httplib → вид ошибки модели.
Turn network_error(httplib::Error err) {
    bool timeout = err == httplib::Error::Read || err == httplib::Error::Timeout;
    return error_turn(timeout ? ModelError::Timeout : ModelError::Unavailable,
                      std::string("network: ") + httplib::to_string(err));
}

void set_timeouts(httplib::Client& cli, int timeout_ms) {
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    cli.set_write_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    cli.set_max_timeout(timeout_ms); // дедлайн на весь запрос
}

} // namespace

// --- OpenAI ---

json openai_request_body(const OpenAiConfig& cfg, const std::string& system, const std::vector<Message>& messages,
                         const std::vector<mcp::Tool>& tools, bool allow_tools) {
    json out = json::array();
    if (!system.empty()) out.push_back({{"role", "system"}, {"content", system}});
    for (const auto& m : messages) {
        switch (m.role) {
        case Role::User: out.push_back({{"role", "user"}, {"content", m.text}}); break;
        case Role::Assistant: {
            json msg = {{"role", "assistant"}};
            msg["content"] = m.text.empty() ? json(nullptr) : json(m.text);
            if (m.raw.is_object() && m.raw.contains("tool_calls") && m.raw["tool_calls"].is_array() &&
                !m.raw["tool_calls"].empty()) {
                msg["tool_calls"] = m.raw["tool_calls"]; // как пришли, вместе с битыми аргументами
            } else if (!m.tool_calls.empty()) {
                json calls = json::array();
                for (const auto& c : m.tool_calls) {
                    calls.push_back({{"id", c.id},
                                     {"type", "function"},
                                     {"function", {{"name", c.name}, {"arguments", c.args.dump()}}}});
                }
                msg["tool_calls"] = calls;
            }
            out.push_back(msg);
            break;
        }
        case Role::Tool:
            for (const auto& r : m.tool_results) {
                out.push_back({{"role", "tool"},
                               {"tool_call_id", r.id},
                               {"content", (r.is_error ? std::string(kOpenAiErrorPrefix) : std::string()) + r.content}});
            }
            if (!m.text.empty()) out.push_back({{"role", "user"}, {"content", m.text}});
            break;
        }
    }
    json body = {{"model", cfg.model}, {"max_tokens", cfg.max_tokens}, {"stream", false}, {"messages", out}};
    if (!cfg.reasoning_effort.empty()) body["reasoning_effort"] = cfg.reasoning_effort;
    if (allow_tools && !tools.empty()) {
        json list = json::array();
        for (const auto& t : tools) {
            list.push_back({{"type", "function"},
                            {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.input_schema}}}});
        }
        body["tools"] = list;
    }
    return body;
}

Turn parse_openai_response(int http_status, const std::string& body) {
    if (http_status == 401 || http_status == 403) return error_turn(ModelError::AuthFailed, "http_" + std::to_string(http_status));
    if (http_status == 404) return error_turn(ModelError::Unavailable, "model_not_loaded");
    if (http_status == 400) return error_turn(ModelError::BadResponse, "http_400");
    if (http_status < 200 || http_status >= 300) return error_turn(ModelError::Unavailable, "http_" + std::to_string(http_status));

    json resp = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!resp.is_object()) return error_turn(ModelError::BadResponse, "response_not_json");
    Usage usage;
    if (resp.contains("usage")) {
        usage.input_tokens = int_field(resp["usage"], "prompt_tokens");
        usage.output_tokens = int_field(resp["usage"], "completion_tokens");
    }
    if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty()) {
        Turn t = error_turn(ModelError::BadResponse, "no_choices");
        t.usage = usage;
        return t;
    }
    const json& choice = resp["choices"][0];
    std::string finish = is_str(choice, "finish_reason") ? choice["finish_reason"].get<std::string>() : "";
    Turn t;
    t.usage = usage;
    t.detail = finish;
    if (finish == "length") {
        t.kind = TurnKind::Truncated;
        return t;
    }
    if (!choice.contains("message") || !choice["message"].is_object()) {
        t = error_turn(ModelError::BadResponse, "no_message");
        t.usage = usage;
        return t;
    }
    const json& msg = choice["message"];
    t.message.role = Role::Assistant;
    if (is_str(msg, "content")) t.message.text = llm::strip_think(msg["content"].get<std::string>());

    if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()) {
        json raw = {{"role", "assistant"}, {"tool_calls", msg["tool_calls"]}};
        int n = 0;
        for (const auto& c : msg["tool_calls"]) {
            ++n;
            ToolCall call;
            call.id = is_str(c, "id") && !c["id"].get<std::string>().empty() ? c["id"].get<std::string>()
                                                                              : "call_" + std::to_string(n);
            const json fn = c.is_object() && c.contains("function") ? c["function"] : json();
            call.name = is_str(fn, "name") ? fn["name"].get<std::string>() : "";
            json args;
            if (is_str(fn, "arguments")) {
                std::string s = fn["arguments"].get<std::string>();
                args = s.empty() ? json::object() : json::parse(s, nullptr, /*allow_exceptions=*/false);
            } else if (fn.is_object() && fn.contains("arguments")) {
                args = fn["arguments"];
            } else {
                args = json::object();
            }
            if (args.is_object()) {
                call.args = args;
            } else {
                call.args_ok = false; // не JSON или не объект — ошибка инструмента для модели
            }
            if (!is_str(c, "id") || c["id"].get<std::string>().empty()) raw["tool_calls"][n - 1]["id"] = call.id;
            t.message.tool_calls.push_back(std::move(call));
        }
        t.message.raw = raw;
        t.kind = TurnKind::ToolCalls;
        return t;
    }
    if (t.message.text.empty()) {
        t = error_turn(ModelError::BadResponse, "empty_text");
        t.usage = usage;
        return t;
    }
    t.message.raw = {{"role", "assistant"}, {"content", t.message.text}};
    t.kind = TurnKind::Text;
    return t;
}

namespace {

class OpenAiChatModel final : public Model {
public:
    explicit OpenAiChatModel(OpenAiConfig cfg) : cfg_(std::move(cfg)), base_(llm::split_base_url(cfg_.url)) {}

    Turn turn(const std::string& system, const std::vector<Message>& messages, const std::vector<mcp::Tool>& tools,
              bool allow_tools, const std::optional<Credential>& credential, int timeout_ms) override {
        if (cfg_.url.empty() || base_.origin.empty()) return error_turn(ModelError::Unavailable, "llm_disabled");
        if (timeout_ms <= 0) return error_turn(ModelError::Timeout, "deadline");
        httplib::Client cli(base_.origin);
        set_timeouts(cli, timeout_ms);
        httplib::Headers headers;
        if (credential && !credential->secret.empty()) headers.emplace("Authorization", "Bearer " + credential->secret);
        auto res = cli.Post(base_.path + "/chat/completions", headers,
                            openai_request_body(cfg_, system, messages, tools, allow_tools).dump(), "application/json");
        if (!res) return network_error(res.error());
        return parse_openai_response(res->status, res->body);
    }

private:
    OpenAiConfig cfg_;
    llm::BaseUrl base_;
};

} // namespace

std::unique_ptr<Model> make_openai_model(OpenAiConfig cfg) {
    return std::make_unique<OpenAiChatModel>(std::move(cfg));
}

// --- Anthropic ---

json anthropic_request_body(const AnthropicConfig& cfg, const std::string& system,
                            const std::vector<Message>& messages, const std::vector<mcp::Tool>& tools,
                            bool allow_tools) {
    json out = json::array();
    for (const auto& m : messages) {
        switch (m.role) {
        case Role::User: out.push_back({{"role", "user"}, {"content", m.text}}); break;
        case Role::Assistant:
            if (m.raw.is_array()) {
                // Append-only: content ответа целиком, вместе с thinking, без
                // правок и перестановок — иначе preserved thinking даёт 400.
                out.push_back({{"role", "assistant"}, {"content", m.raw}});
            } else if (!m.tool_calls.empty()) {
                json blocks = json::array();
                if (!m.text.empty()) blocks.push_back({{"type", "text"}, {"text", m.text}});
                for (const auto& c : m.tool_calls) {
                    blocks.push_back({{"type", "tool_use"}, {"id", c.id}, {"name", c.name}, {"input", c.args}});
                }
                out.push_back({{"role", "assistant"}, {"content", blocks}});
            } else {
                out.push_back({{"role", "assistant"}, {"content", m.text}}); // реплика из истории — только текст
            }
            break;
        case Role::Tool: {
            // Все tool_result одного хода — одним сообщением user.
            json blocks = json::array();
            for (const auto& r : m.tool_results) {
                blocks.push_back({{"type", "tool_result"},
                                  {"tool_use_id", r.id},
                                  {"content", r.content},
                                  {"is_error", r.is_error}});
            }
            if (!m.text.empty()) blocks.push_back({{"type", "text"}, {"text", m.text}});
            out.push_back({{"role", "user"}, {"content", blocks}});
            break;
        }
        }
    }
    json body = {{"model", cfg.model},
                 {"max_tokens", cfg.max_tokens},
                 {"messages", out},
                 {"thinking", {{"type", "adaptive"}}}};
    if (!system.empty()) body["system"] = system;
    if (!tools.empty()) {
        // Схемы передаются и в последнем вызове: в истории есть tool_use.
        // Запрет вызовов — tool_choice none (any/tool на этой модели — 400).
        json list = json::array();
        for (const auto& t : tools) {
            list.push_back({{"name", t.name}, {"description", t.description}, {"input_schema", t.input_schema}});
        }
        body["tools"] = list;
        body["tool_choice"] = {{"type", allow_tools ? "auto" : "none"}};
    }
    if (!cfg.effort.empty()) body["output_config"] = {{"effort", cfg.effort}};
    if (cfg.fallbacks) body["fallbacks"] = "default";
    return body;
}

std::vector<std::pair<std::string, std::string>> anthropic_headers(const AnthropicConfig& cfg,
                                                                   const Credential& credential) {
    std::vector<std::pair<std::string, std::string>> h = {{"x-api-key", credential.secret},
                                                          {"anthropic-version", kAnthropicVersion},
                                                          {"content-type", "application/json"}};
    if (cfg.fallbacks) h.emplace_back("anthropic-beta", kAnthropicFallbackBeta);
    return h;
}

Turn parse_anthropic_response(int http_status, const std::string& body) {
    if (http_status == 401 || http_status == 403) return error_turn(ModelError::AuthFailed, "http_" + std::to_string(http_status));
    if (http_status == 429 || http_status >= 500) {
        return error_turn(ModelError::Unavailable, "http_" + std::to_string(http_status));
    }
    if (http_status < 200 || http_status >= 300) {
        // Тип ошибки — в журнал; message не берём (может цитировать запрос).
        json e = json::parse(body, nullptr, /*allow_exceptions=*/false);
        std::string type = e.is_object() && e.contains("error") && is_str(e["error"], "type")
                               ? " " + e["error"]["type"].get<std::string>()
                               : "";
        return error_turn(ModelError::BadResponse, "http_" + std::to_string(http_status) + type);
    }
    json resp = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!resp.is_object()) return error_turn(ModelError::BadResponse, "response_not_json");
    Usage usage;
    if (resp.contains("usage")) {
        usage.input_tokens = int_field(resp["usage"], "input_tokens");
        usage.output_tokens = int_field(resp["usage"], "output_tokens");
    }
    std::string stop = is_str(resp, "stop_reason") ? resp["stop_reason"].get<std::string>() : "";
    Turn t;
    t.usage = usage;
    t.detail = stop;
    // Отказ — до чтения content.
    if (stop == "refusal") {
        t.kind = TurnKind::Refused;
        if (resp.contains("stop_details") && is_str(resp["stop_details"], "category")) {
            t.detail = "refusal " + resp["stop_details"]["category"].get<std::string>();
        }
        return t;
    }
    if (stop == "max_tokens") {
        t.kind = TurnKind::Truncated;
        return t;
    }
    if (stop != "tool_use" && stop != "end_turn") {
        t = error_turn(ModelError::BadResponse, "stop_reason " + (stop.empty() ? std::string("missing") : stop));
        t.usage = usage;
        return t;
    }
    if (!resp.contains("content") || !resp["content"].is_array()) {
        t = error_turn(ModelError::BadResponse, "no_content");
        t.usage = usage;
        return t;
    }
    t.message.role = Role::Assistant;
    t.message.raw = resp["content"]; // целиком, вместе с thinking (пустой текст — норма)
    for (const auto& b : resp["content"]) {
        std::string type = is_str(b, "type") ? b["type"].get<std::string>() : "";
        if (type == "text" && is_str(b, "text")) {
            if (!t.message.text.empty()) t.message.text += "\n";
            t.message.text += b["text"].get<std::string>();
        } else if (type == "tool_use") {
            ToolCall call;
            call.id = is_str(b, "id") ? b["id"].get<std::string>() : "";
            call.name = is_str(b, "name") ? b["name"].get<std::string>() : "";
            if (b.contains("input") && b["input"].is_object()) {
                call.args = b["input"];
            } else {
                call.args_ok = false;
            }
            t.message.tool_calls.push_back(std::move(call));
        }
    }
    if (stop == "tool_use") {
        if (t.message.tool_calls.empty()) {
            t = error_turn(ModelError::BadResponse, "tool_use_without_blocks");
            t.usage = usage;
            return t;
        }
        t.kind = TurnKind::ToolCalls;
        return t;
    }
    if (t.message.text.empty()) {
        t = error_turn(ModelError::BadResponse, "empty_text");
        t.usage = usage;
        return t;
    }
    t.message.tool_calls.clear();
    t.kind = TurnKind::Text;
    return t;
}

namespace {

class AnthropicChatModel final : public Model {
public:
    explicit AnthropicChatModel(AnthropicConfig cfg) : cfg_(std::move(cfg)) {
        while (!cfg_.base_url.empty() && cfg_.base_url.back() == '/') cfg_.base_url.pop_back();
    }

    Turn turn(const std::string& system, const std::vector<Message>& messages, const std::vector<mcp::Tool>& tools,
              bool allow_tools, const std::optional<Credential>& credential, int timeout_ms) override {
        if (!credential || credential->secret.empty()) return error_turn(ModelError::Unavailable, "no_credential");
        if (timeout_ms <= 0) return error_turn(ModelError::Timeout, "deadline");
        httplib::Client cli(cfg_.base_url);
        set_timeouts(cli, timeout_ms);
        httplib::Headers headers;
        for (const auto& [k, v] : anthropic_headers(cfg_, *credential)) {
            if (k != "content-type") headers.emplace(k, v); // Content-Type ставит Post
        }
        auto res = cli.Post("/v1/messages", headers,
                            anthropic_request_body(cfg_, system, messages, tools, allow_tools).dump(),
                            "application/json");
        if (!res) return network_error(res.error());
        return parse_anthropic_response(res->status, res->body);
    }

private:
    AnthropicConfig cfg_;
};

} // namespace

std::unique_ptr<Model> make_anthropic_model(AnthropicConfig cfg) {
    return std::make_unique<AnthropicChatModel>(std::move(cfg));
}

} // namespace chat
