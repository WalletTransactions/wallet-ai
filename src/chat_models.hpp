// Модели для задания chat (change add-chat-agent, D2): нейтральная модель
// сообщений и адаптеры провайдеров.
//
//   OpenAiChatModel    — lmstudio (и своя модель позже): POST {LLM_URL}/chat/completions
//                        с tools, ответ — choices[0].message.tool_calls;
//   AnthropicChatModel — claude: POST https://api.anthropic.com/v1/messages
//                        сырым HTTPS (у C++ нет официального SDK).
//
// Цикл (chat.cpp) знает только Message/Turn и не знает формы провайдера.
// Разбор ответов и сборка тел — чистые функции, как parse_expense_response.
//
// Ключ провайдера приходит в каждый turn() (Credential, D9): глобального
// клиента с ключом нет. Ни ключ, ни тексты в detail не попадают — detail
// только для журнала вызывающего.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mcp_client.hpp"
#include "nlohmann/json.hpp"

namespace chat {

// Чей ключ: instance — ключ стека (.env), user — собственный ключ
// пользователя (вариант b, пока не используется).
enum class Owner { Instance, User };
const char* to_string(Owner o);

struct Credential {
    std::string secret; // в журнал и в задание не пишется
    Owner owner = Owner::Instance;
};

struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json args = nlohmann::json::object();
    // false — аргументы не JSON-объект (OpenAI отдаёт их строкой): цикл
    // возвращает модели ошибку инструмента, wallet-mcp не вызывается.
    bool args_ok = true;
};

struct ToolResult {
    std::string id; // id вызова (tool_call_id / tool_use_id)
    std::string content;
    bool is_error = false;
};

enum class Role { User, Assistant, Tool };

// Нейтральное сообщение цикла.
//   User      — text;
//   Assistant — text и/или tool_calls; raw — ответ провайдера как есть
//               (Anthropic: весь content вместе с thinking; OpenAI: message),
//               адаптер отдаёт его обратно без изменений. У реплик из
//               истории диалога (D6) raw пуст — только текст;
//   Tool      — tool_results одного хода; text — необязательная приписка
//               (просьба ответить без инструментов после предела шагов).
struct Message {
    Role role = Role::User;
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::vector<ToolResult> tool_results;
    nlohmann::json raw;
};

struct Usage {
    int input_tokens = 0;
    int output_tokens = 0;
};

enum class TurnKind { Text, ToolCalls, Refused, Truncated, Error };
const char* to_string(TurnKind k);

enum class ModelError {
    None,
    AuthFailed,  // 401/403 — ключ отклонён (provider_auth_failed)
    Unavailable, // сеть, 429, 5xx, 529, модель не загружена (unavailable)
    Timeout,     // не уложились в остаток дедлайна (timeout)
    BadResponse, // 400, не JSON, неизвестный stop_reason (internal)
};
const char* to_string(ModelError e);

struct Turn {
    TurnKind kind = TurnKind::Error;
    ModelError error = ModelError::None; // при kind == Error
    Message message;                     // role Assistant: text, tool_calls, raw
    Usage usage;
    std::string detail; // для журнала: stop_reason, категория отказа, http_N
};

class Model {
public:
    virtual ~Model() = default;
    // allow_tools = false — последний вызов после предела шагов: модель
    // должна ответить текстом. timeout_ms — остаток дедлайна задания.
    virtual Turn turn(const std::string& system, const std::vector<Message>& messages,
                      const std::vector<mcp::Tool>& tools, bool allow_tools, const std::optional<Credential>& credential,
                      int timeout_ms) = 0;
};

// --- OpenAI-совместимый chat/completions (lmstudio) ---

struct OpenAiConfig {
    std::string url;   // LLM_URL, http://host:1234/v1
    std::string model; // LLM_MODEL
    int max_tokens = 1024;                // LLM_MAX_TOKENS_CHAT
    std::string reasoning_effort = "none"; // LLM_REASONING_CHAT; пусто — не передаётся
};

// Приписка к ошибке инструмента: у OpenAI-формата нет is_error.
inline constexpr const char* kOpenAiErrorPrefix = "ОШИБКА: ";

nlohmann::json openai_request_body(const OpenAiConfig& cfg, const std::string& system,
                                   const std::vector<Message>& messages, const std::vector<mcp::Tool>& tools,
                                   bool allow_tools);
Turn parse_openai_response(int http_status, const std::string& body);

std::unique_ptr<Model> make_openai_model(OpenAiConfig cfg);

// --- Anthropic Messages API (claude) ---

inline constexpr const char* kAnthropicVersion = "2026-10-21";
inline constexpr const char* kAnthropicFallbackBeta = "server-side-fallback-2026-07-01";

struct AnthropicConfig {
    std::string base_url = "https://api.anthropic.com";
    std::string model = "claude-opus-5-5"; // ANTHROPIC_MODEL
    int max_tokens = 16000;                // ANTHROPIC_MAX_TOKENS
    std::string effort = "low";            // ANTHROPIC_EFFORT
    bool fallbacks = true;                 // ANTHROPIC_FALLBACKS != off
};

// Тело POST /v1/messages: model, max_tokens, system, messages, tools,
// tool_choice auto (none — при allow_tools = false), thinking adaptive,
// output_config.effort, fallbacks "default". Без temperature/top_p/top_k и
// без prefill.
nlohmann::json anthropic_request_body(const AnthropicConfig& cfg, const std::string& system,
                                      const std::vector<Message>& messages, const std::vector<mcp::Tool>& tools,
                                      bool allow_tools);
// Заголовки запроса (x-api-key — из credential).
std::vector<std::pair<std::string, std::string>> anthropic_headers(const AnthropicConfig& cfg,
                                                                   const Credential& credential);
Turn parse_anthropic_response(int http_status, const std::string& body);

std::unique_ptr<Model> make_anthropic_model(AnthropicConfig cfg);

} // namespace chat
