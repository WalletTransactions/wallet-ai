#include "mcp_client.hpp"

#include <atomic>
#include <utility>

#include "httplib.h"
#include "util/log.hpp"

using nlohmann::json;

namespace mcp {

const char* to_string(Status s) {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::AuthExpired: return "auth_expired";
    case Status::ToolsUnavailable: return "tools_unavailable";
    case Status::Timeout: return "timeout";
    case Status::RpcError: return "rpc_error";
    case Status::BadResponse: return "bad_response";
    }
    return "bad_response";
}

const char* failure_code(Status s) {
    switch (s) {
    case Status::Ok: return "";
    case Status::AuthExpired: return "auth_expired";
    case Status::Timeout: return "timeout";
    case Status::ToolsUnavailable:
    case Status::RpcError:
    case Status::BadResponse: return "tools_unavailable";
    }
    return "tools_unavailable";
}

std::string truncate_text(const std::string& text, std::size_t max_chars, bool* truncated) {
    if (truncated != nullptr) *truncated = false;
    if (max_chars == 0) return text;
    // Считаем кодовые точки UTF-8: байт-продолжение 10xxxxxx новую не начинает.
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (chars == max_chars) {
            if (truncated != nullptr) *truncated = true;
            return text.substr(0, i) + kTruncatedMark;
        }
        ++chars;
    }
    return text;
}

Endpoint split_url(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos || scheme_end + 3 >= url.size()) return {};
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return {url, kDefaultPath};
    std::string path = url.substr(path_start);
    if (path == "/") path = kDefaultPath;
    return {url.substr(0, path_start), path};
}

Outcome parse_rpc_response(int http_status, const std::string& content_type, const std::string& body,
                           const json& expected_id, json& result) {
    Outcome out;
    out.http_status = http_status;
    if (http_status == 401) {
        out.status = Status::AuthExpired;
        out.detail = "http_401";
        return out;
    }
    if (http_status >= 500) {
        out.status = Status::ToolsUnavailable;
        out.detail = "http_" + std::to_string(http_status);
        return out;
    }
    // wallet-mcp отвечает одним JSON; поток событий клиент не читает.
    if (content_type.rfind("text/event-stream", 0) == 0) {
        out.status = Status::BadResponse;
        out.detail = "sse_not_supported";
        return out;
    }
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    // JSON-RPC error бывает и с 400 (незнакомая MCP-Protocol-Version).
    if (j.is_object() && j.contains("error") && j["error"].is_object()) {
        const auto& e = j["error"];
        out.status = Status::RpcError;
        out.rpc_code = e.contains("code") && e["code"].is_number_integer() ? e["code"].get<int>() : 0;
        out.message = e.contains("message") && e["message"].is_string() ? e["message"].get<std::string>() : "";
        out.detail = "rpc_error " + std::to_string(out.rpc_code);
        return out;
    }
    if (http_status != 200) {
        out.status = Status::BadResponse;
        out.detail = "http_" + std::to_string(http_status);
        return out;
    }
    if (!j.is_object()) {
        out.status = Status::BadResponse;
        out.detail = "not_json";
        return out;
    }
    if (!j.contains("id") || j["id"] != expected_id) {
        out.status = Status::BadResponse;
        out.detail = "id_mismatch";
        return out;
    }
    if (!j.contains("result") || !j["result"].is_object()) {
        out.status = Status::BadResponse;
        out.detail = "no_result";
        return out;
    }
    result = j["result"];
    out.status = Status::Ok;
    return out;
}

InitResult parse_initialize_result(const json& result) {
    InitResult out;
    out.status = Status::BadResponse;
    if (!result.is_object() || !result.contains("protocolVersion") || !result["protocolVersion"].is_string() ||
        result["protocolVersion"].get<std::string>().empty()) {
        out.detail = "initialize: no protocolVersion";
        return out;
    }
    out.protocol_version = result["protocolVersion"].get<std::string>();
    if (result.contains("instructions") && result["instructions"].is_string()) {
        out.instructions = result["instructions"].get<std::string>();
    }
    if (result.contains("serverInfo") && result["serverInfo"].is_object()) {
        const auto& si = result["serverInfo"];
        if (si.contains("name") && si["name"].is_string()) out.server_name = si["name"].get<std::string>();
        if (si.contains("version") && si["version"].is_string()) out.server_version = si["version"].get<std::string>();
    }
    out.status = Status::Ok;
    return out;
}

ToolsResult parse_tools_list_result(const json& result) {
    ToolsResult out;
    out.status = Status::BadResponse;
    if (!result.is_object() || !result.contains("tools") || !result["tools"].is_array()) {
        out.detail = "tools/list: no tools";
        return out;
    }
    for (const auto& t : result["tools"]) {
        // Инструмент без имени модели не предложить — пропускаем.
        if (!t.is_object() || !t.contains("name") || !t["name"].is_string()) continue;
        Tool tool;
        tool.name = t["name"].get<std::string>();
        if (tool.name.empty()) continue;
        if (t.contains("description") && t["description"].is_string()) {
            tool.description = t["description"].get<std::string>();
        }
        if (t.contains("inputSchema") && t["inputSchema"].is_object()) {
            tool.input_schema = t["inputSchema"];
        } else {
            tool.input_schema = json{{"type", "object"}};
        }
        out.tools.push_back(std::move(tool));
    }
    out.status = Status::Ok;
    return out;
}

CallResult parse_tool_call_result(const json& result, std::size_t max_chars) {
    CallResult out;
    out.status = Status::BadResponse;
    if (!result.is_object() || !result.contains("content") || !result["content"].is_array()) {
        out.detail = "tools/call: no content";
        return out;
    }
    std::string text;
    bool any = false;
    for (const auto& c : result["content"]) {
        if (!c.is_object() || c.value("type", std::string{}) != "text" || !c.contains("text") ||
            !c["text"].is_string()) {
            continue;
        }
        if (any) text += "\n";
        text += c["text"].get<std::string>();
        any = true;
    }
    // Только structuredContent без текста — отдаём его JSON-строкой.
    if (!any && result.contains("structuredContent") && !result["structuredContent"].is_null()) {
        text = result["structuredContent"].dump();
    }
    out.is_error = result.contains("isError") && result["isError"].is_boolean() && result["isError"].get<bool>();
    out.content = truncate_text(text, max_chars, &out.truncated);
    out.status = Status::Ok;
    return out;
}

namespace {

// Сколько страниц tools/list читать по nextCursor (wallet-mcp отдаёт одну).
constexpr int kMaxToolPages = 10;

class HttpClient final : public Client {
public:
    explicit HttpClient(Config cfg) : cfg_(std::move(cfg)), endpoint_(split_url(cfg_.url)) {}

    InitResult initialize(Session& session, int timeout_ms) override {
        json params = {{"protocolVersion", kProtocolVersion},
                       {"capabilities", json::object()},
                       {"clientInfo", {{"name", "wallet-ai"}, {"version", "1"}}}};
        json result;
        std::string session_id;
        Outcome o = rpc(session, "initialize", params, /*after_init=*/false, timeout_ms, result, &session_id);
        if (o.status != Status::Ok) return with(InitResult{}, o);
        InitResult r = parse_initialize_result(result);
        r.http_status = o.http_status;
        if (r.status != Status::Ok) return r;
        session.protocol_version = r.protocol_version;
        session.session_id = session_id;

        // Уведомление обязательно по протоколу; wallet-mcp отвечает 202 без тела.
        Outcome n = notify(session, "notifications/initialized", timeout_ms);
        if (n.status != Status::Ok) return with(InitResult{}, n);
        return r;
    }

    ToolsResult list_tools(const Session& session, int timeout_ms) override {
        ToolsResult all;
        json params = json::object();
        for (int page = 0; page < kMaxToolPages; ++page) {
            json result;
            Outcome o = rpc(session, "tools/list", params, /*after_init=*/true, timeout_ms, result, nullptr);
            if (o.status != Status::Ok) return with(ToolsResult{}, o);
            ToolsResult r = parse_tools_list_result(result);
            r.http_status = o.http_status;
            if (r.status != Status::Ok) return r;
            for (auto& t : r.tools) all.tools.push_back(std::move(t));
            all.http_status = r.http_status;
            if (!result.contains("nextCursor") || !result["nextCursor"].is_string() ||
                result["nextCursor"].get<std::string>().empty()) {
                break;
            }
            params = {{"cursor", result["nextCursor"]}};
        }
        all.status = Status::Ok;
        return all;
    }

    CallResult call_tool(const Session& session, const std::string& name, const json& arguments,
                         std::size_t max_chars, int timeout_ms) override {
        json params = {{"name", name}, {"arguments", arguments.is_object() ? arguments : json::object()}};
        json result;
        Outcome o = rpc(session, "tools/call", params, /*after_init=*/true, timeout_ms, result, nullptr);
        if (o.status != Status::Ok) return with(CallResult{}, o);
        CallResult r = parse_tool_call_result(result, max_chars);
        r.http_status = o.http_status;
        return r;
    }

private:
    template <class R>
    static R with(R r, const Outcome& o) {
        static_cast<Outcome&>(r) = o;
        return r;
    }

    httplib::Headers headers(const Session& session, bool after_init) const {
        httplib::Headers h;
        h.emplace("Authorization", "Bearer " + session.token);
        h.emplace("Accept", "application/json, text/event-stream");
        if (after_init) {
            h.emplace("MCP-Protocol-Version",
                      session.protocol_version.empty() ? std::string(kProtocolVersion) : session.protocol_version);
        }
        if (!session.session_id.empty()) h.emplace("Mcp-Session-Id", session.session_id);
        std::string trace_id = applog::current_trace_id();
        if (!trace_id.empty()) h.emplace("X-Trace-Id", trace_id);
        return h;
    }

    // POST одного сообщения. nullopt-подобный исход — Outcome со статусом
    // сети; иначе — ответ сервера.
    bool post(const Session& session, const json& msg, bool after_init, int timeout_ms, Outcome& out,
              httplib::Response& response) {
        if (endpoint_.origin.empty()) {
            out.status = Status::ToolsUnavailable;
            out.detail = "mcp_url_not_set";
            return false;
        }
        if (timeout_ms <= 0) {
            out.status = Status::Timeout;
            out.detail = "deadline";
            return false;
        }
        // Клиент на вызов: запросы редкие (несколько на задание), по
        // внутренней сети, а клиент общий для потоков воркеров.
        httplib::Client cli(endpoint_.origin);
        cli.set_connection_timeout(cfg_.connect_timeout_ms / 1000, (cfg_.connect_timeout_ms % 1000) * 1000);
        cli.set_read_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
        cli.set_write_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
        cli.set_max_timeout(timeout_ms); // дедлайн на весь запрос

        auto res = cli.Post(endpoint_.path, headers(session, after_init), msg.dump(), "application/json");
        if (!res) {
            auto err = res.error();
            out.status = (err == httplib::Error::Read || err == httplib::Error::Timeout) ? Status::Timeout
                                                                                       : Status::ToolsUnavailable;
            out.detail = std::string("network: ") + httplib::to_string(err);
            return false;
        }
        response = *res;
        return true;
    }

    Outcome rpc(const Session& session, const std::string& method, const json& params, bool after_init,
                int timeout_ms, json& result, std::string* session_id) {
        json id = next_id_.fetch_add(1);
        json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
        Outcome out;
        httplib::Response res;
        if (!post(session, msg, after_init, timeout_ms, out, res)) return out;
        out = parse_rpc_response(res.status, res.get_header_value("Content-Type"), res.body, id, result);
        if (out.status != Status::Ok) out.detail = method + ": " + out.detail;
        if (session_id != nullptr) *session_id = res.get_header_value("Mcp-Session-Id");
        return out;
    }

    Outcome notify(const Session& session, const std::string& method, int timeout_ms) {
        json msg = {{"jsonrpc", "2.0"}, {"method", method}};
        Outcome out;
        httplib::Response res;
        if (!post(session, msg, /*after_init=*/true, timeout_ms, out, res)) return out;
        out.http_status = res.status;
        if (res.status == 202 || res.status == 200) {
            out.status = Status::Ok;
            return out;
        }
        json ignored;
        out = parse_rpc_response(res.status, res.get_header_value("Content-Type"), res.body, nullptr, ignored);
        if (out.status == Status::Ok) out.status = Status::BadResponse; // неожиданный не-2xx
        out.detail = method + ": " + out.detail;
        return out;
    }

    Config cfg_;
    Endpoint endpoint_;
    std::atomic<long long> next_id_{1};
};

} // namespace

std::unique_ptr<Client> make_http_client(Config cfg) { return std::make_unique<HttpClient>(std::move(cfg)); }

} // namespace mcp
