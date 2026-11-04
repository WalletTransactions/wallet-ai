// MCP-клиент к wallet-mcp (change add-chat-agent, D3): Streamable HTTP,
// JSON-RPC 2.0, один POST — один JSON-ответ (SSE wallet-mcp не открывает).
//
//   POST {WALLET_MCP_URL} (http://wallet-mcp:8088/api/mcp/, напрямую, не через
//   гейтвей), заголовки: Authorization: Bearer <токен обмена, auth_exchange>,
//   Content-Type: application/json, Accept: application/json, text/event-stream,
//   MCP-Protocol-Version — на всех запросах после initialize.
//
// На каждое задание chat — initialize (берутся instructions), затем
// tools/list и tools/call. wallet-mcp сессий не хранит, поэтому клиент без
// состояния и общий для всех заданий; всё, что относится к одному заданию
// (токен, Mcp-Session-Id, согласованная версия), — в mcp::Session у
// вызывающего.
//
// Отображение ошибок (spec ai-chat, «Отказы задания chat»):
//   HTTP 401              → AuthExpired      (failure auth_expired);
//   503, 5xx, сеть        → ToolsUnavailable (failure tools_unavailable);
//   таймаут запроса       → Timeout          (failure timeout — дедлайн задания);
//   JSON-RPC error        → RpcError с кодом и сообщением;
//   прочий HTTP, не JSON  → BadResponse.
// isError в результате tools/call — не ошибка клиента: Ok с is_error, текст
// уходит модели как результат инструмента (D2: tool_results[].is_error).
//
// Клиент ничего не пишет в журнал. Токен, аргументы и результаты
// инструментов не попадают и в detail — он только для журнала вызывающего.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mcp {

inline constexpr const char* kProtocolVersion = "2025-11-25";
inline constexpr const char* kDefaultPath = "/api/mcp/";
// Пометка обрезанного результата инструмента (AI_CHAT_TOOL_RESULT_MAX_CHARS).
inline constexpr const char* kTruncatedMark = "…(обрезано)";

enum class Status {
    Ok,
    AuthExpired,
    ToolsUnavailable,
    Timeout,
    RpcError,
    BadResponse,
};

const char* to_string(Status s);

// failure задания chat, если вызов не удался: auth_expired, timeout или
// tools_unavailable (RpcError/BadResponse на initialize и tools/list —
// wallet-mcp непригоден). Для Ok — пусто. RpcError у tools/call цикл
// обычно не валит, а отдаёт сообщение модели ошибкой инструмента.
const char* failure_code(Status s);

// Состояние одного задания. Токен — только здесь, в памяти задания.
struct Session {
    std::string token;
    std::string session_id;       // Mcp-Session-Id из ответа initialize, если пришёл
    std::string protocol_version; // согласованная в initialize; пусто — kProtocolVersion
};

// Общая часть результатов.
struct Outcome {
    Status status = Status::ToolsUnavailable;
    int http_status = 0;   // 0 — до HTTP-ответа не дошло
    int rpc_code = 0;      // при RpcError
    std::string message;   // при RpcError — error.message из wallet-mcp
    std::string detail;    // для журнала: что не так (без токена и данных)
};

struct InitResult : Outcome {
    std::string instructions;
    std::string protocol_version;
    std::string server_name;
    std::string server_version;
};

// Описание инструмента из tools/list — в нейтральной форме (D2): адаптер
// модели сам переводит input_schema в parameters (OpenAI) или input_schema
// (Anthropic).
struct Tool {
    std::string name;
    std::string description;
    nlohmann::json input_schema = nlohmann::json::object();
};

struct ToolsResult : Outcome {
    std::vector<Tool> tools;
};

struct CallResult : Outcome {
    std::string content;    // текстовые блоки content через \n, после обрезки
    bool is_error = false;  // isError wallet-mcp — ошибка инструмента для модели
    bool truncated = false;
};

// --- Чистые функции (юнит-тесты) ---

// Разбор HTTP-ответа на JSON-RPC запрос с id expected_id. При Ok в result —
// поле result ответа.
Outcome parse_rpc_response(int http_status, const std::string& content_type, const std::string& body,
                           const nlohmann::json& expected_id, nlohmann::json& result);

InitResult parse_initialize_result(const nlohmann::json& result);
ToolsResult parse_tools_list_result(const nlohmann::json& result);
// max_chars — AI_CHAT_TOOL_RESULT_MAX_CHARS; 0 — без обрезки.
CallResult parse_tool_call_result(const nlohmann::json& result, std::size_t max_chars);

// Первые max_chars символов (кодовых точек UTF-8) + kTruncatedMark, если
// текст длиннее; иначе как есть. max_chars 0 — без обрезки.
std::string truncate_text(const std::string& text, std::size_t max_chars, bool* truncated = nullptr);

// "http://wallet-mcp:8088/api/mcp/" → {"http://wallet-mcp:8088", "/api/mcp/"};
// без пути — kDefaultPath. Не URL — пустой origin.
struct Endpoint {
    std::string origin;
    std::string path;
};
Endpoint split_url(const std::string& url);

class Client {
public:
    virtual ~Client() = default;
    // initialize + notifications/initialized. Заполняет session.session_id и
    // session.protocol_version. timeout_ms — на каждый HTTP-запрос (не больше
    // остатка дедлайна задания).
    virtual InitResult initialize(Session& session, int timeout_ms) = 0;
    virtual ToolsResult list_tools(const Session& session, int timeout_ms) = 0;
    virtual CallResult call_tool(const Session& session, const std::string& name, const nlohmann::json& arguments,
                                 std::size_t max_chars, int timeout_ms) = 0;
};

struct Config {
    std::string url; // WALLET_MCP_URL
    int connect_timeout_ms = 2000;
};

// Пустой или негодный url — все вызовы сразу ToolsUnavailable без сети.
std::unique_ptr<Client> make_http_client(Config cfg);

} // namespace mcp
