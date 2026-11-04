#include "mcp_client.hpp"

#include <atomic>

#include "fake_http.hpp"
#include "test_framework.hpp"

using nlohmann::json;

namespace {

const char* kToken = "eyJ.mcp-token-for-test.sig";
constexpr int kTimeoutMs = 3000;

json rpc_result(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpc_error(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json sample_tools() {
    return {{"tools",
             json::array({{{"name", "spending_by_category"},
                           {"description", "Расходы по категориям за период"},
                           {"inputSchema",
                            {{"type", "object"},
                             {"properties", {{"from", {{"type", "string"}}}, {"to", {{"type", "string"}}}}},
                             {"required", json::array({"from", "to"})}}}},
                          {{"name", "list_categories"}, {"description", "Категории"}}})}};
}

json tool_text(const std::string& text, bool is_error) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", is_error}};
}

// Фейк wallet-mcp: тот же контракт, что src/mcp_http.cpp + src/mcp.cpp
// wallet-mcp — проверка токена (401), 202 на уведомления, JSON-RPC ответы.
struct FakeMcp {
    std::atomic<int> http_status{0}; // != 0 — ответить этим кодом без тела RPC
    std::string tool_result_text = "Няня: 12 000 RUB";
    bool tool_is_error = false;

    testing::FakeHttpServer server{mcp::kDefaultPath, [this](const httplib::Request& req, httplib::Response& res) {
                                       handle(req, res);
                                   }};

    std::string url() { return server.origin() + mcp::kDefaultPath; }

    void handle(const httplib::Request& req, httplib::Response& res) {
        if (http_status != 0) {
            res.status = http_status;
            if (http_status == 401) {
                res.set_header("WWW-Authenticate", "Bearer");
                res.set_content(R"({"error":"invalid_token"})", "application/json");
            } else {
                res.set_content(R"({"error":"unavailable"})", "application/json");
            }
            return;
        }
        if (req.get_header_value("Authorization") != std::string("Bearer ") + kToken) {
            res.status = 401;
            res.set_content(R"({"error":"invalid_token"})", "application/json");
            return;
        }
        json msg = json::parse(req.body);
        if (!msg.contains("id")) {
            res.status = 202;
            return;
        }
        const json id = msg["id"];
        const std::string method = msg["method"];
        json reply;
        if (method == "initialize") {
            reply = rpc_result(id, {{"protocolVersion", msg["params"]["protocolVersion"]},
                                    {"capabilities", {{"tools", json::object()}}},
                                    {"serverInfo", {{"name", "wallet-mcp"}, {"version", "0.3.0"}}},
                                    {"instructions", "Сегодня 2026-11-04. Периоды — явными датами."}});
            res.set_header("Mcp-Session-Id", "sess-42");
        } else if (method == "tools/list") {
            reply = rpc_result(id, sample_tools());
        } else if (method == "tools/call") {
            const std::string name = msg["params"]["name"];
            if (name == "nope") {
                reply = rpc_error(id, -32602, "Неизвестный инструмент: nope");
            } else {
                reply = rpc_result(id, tool_text(tool_result_text, tool_is_error));
            }
        } else {
            reply = rpc_error(id, -32601, "Method not found: " + method);
        }
        res.set_content(reply.dump(), "application/json");
    }
};

mcp::Session session() {
    mcp::Session s;
    s.token = kToken;
    return s;
}

} // namespace

// --- Чистые функции ---

TEST(mcp_truncate_text_utf8) {
    bool truncated = true;
    CHECK_EQ(mcp::truncate_text("короткий", 100, &truncated), std::string("короткий"));
    CHECK(!truncated);
    CHECK_EQ(mcp::truncate_text("абвгд", 5, &truncated), std::string("абвгд"));
    CHECK(!truncated);
    CHECK_EQ(mcp::truncate_text("абвгдеж", 3, &truncated), std::string("абв") + mcp::kTruncatedMark);
    CHECK(truncated);
    CHECK_EQ(mcp::truncate_text("abcdef", 0, &truncated), std::string("abcdef"));
    CHECK(!truncated);
}

TEST(mcp_split_url) {
    auto e = mcp::split_url("http://wallet-mcp:8088/api/mcp/");
    CHECK_EQ(e.origin, std::string("http://wallet-mcp:8088"));
    CHECK_EQ(e.path, std::string("/api/mcp/"));
    CHECK_EQ(mcp::split_url("http://wallet-mcp:8088").path, std::string("/api/mcp/"));
    CHECK_EQ(mcp::split_url("http://wallet-mcp:8088/").path, std::string("/api/mcp/"));
    CHECK(mcp::split_url("wallet-mcp").origin.empty());
    CHECK(mcp::split_url("").origin.empty());
}

TEST(mcp_parse_rpc_response_statuses) {
    json result;
    CHECK(mcp::parse_rpc_response(401, "application/json", R"({"error":"invalid_token"})", 1, result).status ==
          mcp::Status::AuthExpired);
    CHECK(mcp::parse_rpc_response(503, "application/json", R"({"error":"unavailable"})", 1, result).status ==
          mcp::Status::ToolsUnavailable);
    CHECK(mcp::parse_rpc_response(502, "", "", 1, result).status == mcp::Status::ToolsUnavailable);
    CHECK(mcp::parse_rpc_response(200, "application/json", "не json", 1, result).status ==
          mcp::Status::BadResponse);
    CHECK(mcp::parse_rpc_response(200, "text/event-stream", "data: {}\n\n", 1, result).status ==
          mcp::Status::BadResponse);
    CHECK(mcp::parse_rpc_response(200, "application/json", rpc_result(2, json::object()).dump(), 1, result).status ==
          mcp::Status::BadResponse); // чужой id
    CHECK(mcp::parse_rpc_response(404, "text/plain", "Not Found", 1, result).status == mcp::Status::BadResponse);

    auto err = mcp::parse_rpc_response(200, "application/json", rpc_error(1, -32602, "плохие аргументы").dump(), 1,
                                       result);
    CHECK(err.status == mcp::Status::RpcError);
    CHECK_EQ(err.rpc_code, -32602);
    CHECK_EQ(err.message, std::string("плохие аргументы"));
    // 400 с JSON-RPC error (незнакомая MCP-Protocol-Version) — тоже RpcError.
    CHECK(mcp::parse_rpc_response(400, "application/json", rpc_error(nullptr, -32000, "Bad Request").dump(), 1,
                                  result)
              .status == mcp::Status::RpcError);

    auto ok = mcp::parse_rpc_response(200, "application/json", rpc_result(7, {{"x", 1}}).dump(), 7, result);
    CHECK(ok.status == mcp::Status::Ok);
    CHECK_EQ(result["x"].get<int>(), 1);
}

TEST(mcp_failure_codes) {
    CHECK_EQ(std::string(mcp::failure_code(mcp::Status::AuthExpired)), std::string("auth_expired"));
    CHECK_EQ(std::string(mcp::failure_code(mcp::Status::ToolsUnavailable)), std::string("tools_unavailable"));
    CHECK_EQ(std::string(mcp::failure_code(mcp::Status::BadResponse)), std::string("tools_unavailable"));
    CHECK_EQ(std::string(mcp::failure_code(mcp::Status::Timeout)), std::string("timeout"));
    CHECK_EQ(std::string(mcp::failure_code(mcp::Status::Ok)), std::string(""));
}

TEST(mcp_parse_tool_call_result) {
    json two = {{"content", json::array({{{"type", "text"}, {"text", "a"}},
                                         {{"type", "image"}, {"data", "…"}},
                                         {{"type", "text"}, {"text", "b"}}})}};
    auto r = mcp::parse_tool_call_result(two, 0);
    REQUIRE(r.status == mcp::Status::Ok);
    CHECK_EQ(r.content, std::string("a\nb"));
    CHECK(!r.is_error);
    CHECK(mcp::parse_tool_call_result(json::object(), 0).status == mcp::Status::BadResponse);
    auto err = mcp::parse_tool_call_result(tool_text("Нет категории «Няня». Похожие: Baby Sitter", true), 0);
    CHECK(err.is_error);
}

TEST(mcp_parse_tools_list_skips_nameless) {
    json list = sample_tools();
    list["tools"].push_back({{"description", "без имени"}});
    auto r = mcp::parse_tools_list_result(list);
    REQUIRE(r.status == mcp::Status::Ok);
    REQUIRE(r.tools.size() == 2u);
    CHECK_EQ(r.tools[0].input_schema["required"].size(), 2u);
    CHECK_EQ(r.tools[1].input_schema["type"].get<std::string>(), std::string("object"));
    CHECK(mcp::parse_tools_list_result(json::object()).status == mcp::Status::BadResponse);
}

// --- Клиент против фейкового wallet-mcp ---

TEST(mcp_client_initialize_list_call) {
    FakeMcp fake;
    auto client = mcp::make_http_client({fake.url()});
    auto s = session();

    std::string logged;
    mcp::InitResult init;
    mcp::ToolsResult tools;
    mcp::CallResult call;
    {
        testing::CaptureOutput capture;
        init = client->initialize(s, kTimeoutMs);
        tools = client->list_tools(s, kTimeoutMs);
        call = client->call_tool(s, "spending_by_category", {{"from", "2026-09-01"}, {"to", "2026-09-30"}}, 6000,
                                 kTimeoutMs);
        logged = capture.text();
    }

    REQUIRE(init.status == mcp::Status::Ok);
    CHECK_EQ(init.instructions, std::string("Сегодня 2026-11-04. Периоды — явными датами."));
    CHECK_EQ(init.protocol_version, std::string("2025-11-25"));
    CHECK_EQ(init.server_name, std::string("wallet-mcp"));
    CHECK_EQ(s.session_id, std::string("sess-42"));
    CHECK_EQ(s.protocol_version, std::string("2025-11-25"));

    REQUIRE(tools.status == mcp::Status::Ok);
    REQUIRE(tools.tools.size() == 2u);
    CHECK_EQ(tools.tools[0].name, std::string("spending_by_category"));
    CHECK_EQ(tools.tools[0].description, std::string("Расходы по категориям за период"));
    CHECK_EQ(tools.tools[0].input_schema["properties"]["from"]["type"].get<std::string>(), std::string("string"));

    REQUIRE(call.status == mcp::Status::Ok);
    CHECK_EQ(call.content, std::string("Няня: 12 000 RUB"));
    CHECK(!call.is_error);
    CHECK(!call.truncated);

    // initialize, notifications/initialized, tools/list, tools/call.
    auto reqs = fake.server.requests();
    REQUIRE(reqs.size() == 4u);
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        const auto& r = reqs[i];
        CHECK_EQ(r.get_header_value("Authorization"), std::string("Bearer ") + kToken);
        CHECK(r.get_header_value("Content-Type").rfind("application/json", 0) == 0);
        CHECK_EQ(r.get_header_value("Accept"), std::string("application/json, text/event-stream"));
        if (i == 0) {
            CHECK(!r.has_header("MCP-Protocol-Version"));
            auto body = json::parse(r.body);
            CHECK_EQ(body["method"].get<std::string>(), std::string("initialize"));
            CHECK_EQ(body["params"]["protocolVersion"].get<std::string>(), std::string("2025-11-25"));
        } else {
            CHECK_EQ(r.get_header_value("MCP-Protocol-Version"), std::string("2025-11-25"));
            CHECK_EQ(r.get_header_value("Mcp-Session-Id"), std::string("sess-42"));
        }
    }
    CHECK_EQ(json::parse(reqs[1].body)["method"].get<std::string>(), std::string("notifications/initialized"));
    auto call_body = json::parse(reqs[3].body);
    CHECK_EQ(call_body["jsonrpc"].get<std::string>(), std::string("2.0"));
    CHECK_EQ(call_body["params"]["name"].get<std::string>(), std::string("spending_by_category"));
    CHECK_EQ(call_body["params"]["arguments"]["from"].get<std::string>(), std::string("2026-09-01"));

    // Токен не попадает ни в журнал, ни в detail.
    CHECK(logged.find(kToken) == std::string::npos);
    CHECK(init.detail.find(kToken) == std::string::npos);
}

TEST(mcp_client_tool_is_error_and_rpc_error) {
    FakeMcp fake;
    fake.tool_result_text = "Нет категории «Няня». Похожие: Baby Sitter";
    fake.tool_is_error = true;
    auto client = mcp::make_http_client({fake.url()});
    auto s = session();

    auto r = client->call_tool(s, "spending_by_category", {{"category", "Няня"}}, 6000, kTimeoutMs);
    REQUIRE(r.status == mcp::Status::Ok);
    CHECK(r.is_error);
    CHECK_EQ(r.content, std::string("Нет категории «Няня». Похожие: Baby Sitter"));

    auto e = client->call_tool(s, "nope", json::object(), 6000, kTimeoutMs);
    CHECK(e.status == mcp::Status::RpcError);
    CHECK_EQ(e.rpc_code, -32602);
    CHECK_EQ(e.message, std::string("Неизвестный инструмент: nope"));
}

TEST(mcp_client_truncates_long_result) {
    FakeMcp fake;
    fake.tool_result_text = std::string(7000, 'x');
    auto client = mcp::make_http_client({fake.url()});
    auto r = client->call_tool(session(), "search_operations", json::object(), 6000, kTimeoutMs);
    REQUIRE(r.status == mcp::Status::Ok);
    CHECK(r.truncated);
    CHECK_EQ(r.content, std::string(6000, 'x') + mcp::kTruncatedMark);
}

TEST(mcp_client_401_auth_expired) {
    FakeMcp fake;
    auto client = mcp::make_http_client({fake.url()});
    mcp::Session wrong;
    wrong.token = "eyJ.expired.sig";
    auto init = client->initialize(wrong, kTimeoutMs);
    CHECK(init.status == mcp::Status::AuthExpired);
    CHECK_EQ(init.http_status, 401);
    CHECK_EQ(std::string(mcp::failure_code(init.status)), std::string("auth_expired"));

    fake.http_status = 401;
    auto call = client->call_tool(session(), "spending_by_category", json::object(), 6000, kTimeoutMs);
    CHECK(call.status == mcp::Status::AuthExpired);
}

TEST(mcp_client_503_and_closed_port_tools_unavailable) {
    FakeMcp fake;
    fake.http_status = 503;
    auto client = mcp::make_http_client({fake.url()});
    auto s = session();
    auto list = client->list_tools(s, kTimeoutMs);
    CHECK(list.status == mcp::Status::ToolsUnavailable);
    CHECK_EQ(list.http_status, 503);
    CHECK_EQ(std::string(mcp::failure_code(list.status)), std::string("tools_unavailable"));

    auto closed = mcp::make_http_client(
        {"http://127.0.0.1:" + std::to_string(testing::closed_port()) + mcp::kDefaultPath});
    auto init = closed->initialize(s, kTimeoutMs);
    CHECK(init.status == mcp::Status::ToolsUnavailable);
    CHECK_EQ(init.http_status, 0);
    CHECK(init.detail.find(kToken) == std::string::npos);

    // Без WALLET_MCP_URL — сразу tools_unavailable, без сети.
    CHECK(mcp::make_http_client({""})->list_tools(s, kTimeoutMs).status == mcp::Status::ToolsUnavailable);
    // Дедлайн уже истёк — timeout, запрос не уходит.
    CHECK(client->list_tools(s, 0).status == mcp::Status::Timeout);
}
