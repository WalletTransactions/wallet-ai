// GET /api/ai/jobs/{id}/events (change chat-sse-events, tasks 4.1–4.5): поток
// на настоящем httplib-сервере; аутентификацию заменяет заголовок X-User.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "ai.hpp"
#include "app_state.hpp"
#include "httplib.h"
#include "test_framework.hpp"

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

struct Stand {
    AppState state;
    httplib::Server svr;
    std::thread thread;
    int port = 0;

    explicit Stand(int max_total = 5, int max_per_user = 3, bool enabled = true) {
        state.jobs = std::make_unique<jobs::Store>(jobs::Limits{});
        state.streams = std::make_unique<aiapi::StreamGate>(max_total, max_per_user);
        state.stream_enabled = enabled;
        state.stream_ping = 60ms;
        svr.Get(R"(/events/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::size_t after = 0;
            std::string h = req.get_header_value("Last-Event-ID");
            if (!h.empty()) after = static_cast<std::size_t>(std::stoull(h));
            std::chrono::milliseconds window{0};
            if (req.has_param("window_ms")) window = std::chrono::milliseconds(std::stoi(req.get_param_value("window_ms")));
            aiapi::stream_events(state, req.matches[1], req.get_header_value("X-User"), after, res, window);
        });
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    ~Stand() {
        state.jobs->stop();
        svr.stop();
        if (thread.joinable()) thread.join();
    }

    std::string chat_job(const std::string& user = "u1", const std::string& kind = "chat") {
        auto c = state.jobs->create(user, kind, jobs::Priority::Interactive, json{{"text", "q"}}, "", jobs::Lane::Cloud);
        state.jobs->take(jobs::Lane::Cloud);
        return c.job.id;
    }

    struct Reply {
        int status = 0;
        std::string content_type;
        std::string body;
    };
    Reply get(const std::string& id, const std::string& user = "u1", const std::string& last_id = "",
              const std::string& query = "") {
        httplib::Client cli("127.0.0.1", port);
        cli.set_read_timeout(5, 0);
        httplib::Headers h = {{"X-User", user}};
        if (!last_id.empty()) h.emplace("Last-Event-ID", last_id);
        Reply r;
        auto res = cli.Get("/events/" + id + query, h, [&](const httplib::Response& resp) {
            r.status = resp.status;
            r.content_type = resp.get_header_value("Content-Type");
            return true;
        }, [&](const char* d, std::size_t n) {
            r.body.append(d, n);
            return true;
        });
        if (!res) r.status = -1;
        return r;
    }
};

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

} // namespace

TEST(events_route_streams_until_done) {
    Stand st;
    auto id = st.chat_job();
    st.state.jobs->append_event(id, "tool", {{"name", "balance"}, {"outcome", "ok"}});
    std::thread producer([&] {
        std::this_thread::sleep_for(100ms);
        st.state.jobs->append_event(id, "delta", {{"text", "Привет"}});
        st.state.jobs->complete(id, jobs::Status::Done, json{{"text", "Привет"}}, "");
    });
    auto r = st.get(id);
    producer.join();
    CHECK_EQ(r.status, 200);
    CHECK(has(r.content_type, "text/event-stream"));
    CHECK(has(r.body, "id: 1\nevent: tool\ndata: {"));
    CHECK(has(r.body, "event: delta\ndata: {\"text\":\"Привет\"}"));
    CHECK(has(r.body, "event: done"));
    CHECK(r.body.find("event: done") > r.body.find("event: delta")); // порядок сохранён
    CHECK(has(r.body, ": ping")); // пока producer молчал, шёл пульс
}

TEST(events_route_resumes_after_last_event_id) {
    Stand st;
    auto id = st.chat_job();
    st.state.jobs->append_event(id, "tool", {{"name", "a"}, {"outcome", "ok"}});
    st.state.jobs->read_events(id, "u1", 0); // tool отдан
    st.state.jobs->append_event(id, "delta", {{"text", "хвост"}});
    st.state.jobs->complete(id, jobs::Status::Done, json{{"text", "хвост"}}, "");
    auto r = st.get(id, "u1", "1"); // Last-Event-ID: 1
    CHECK_EQ(r.status, 200);
    CHECK(!has(r.body, "event: tool")); // без повторов
    CHECK(has(r.body, "id: 2\nevent: delta"));
    CHECK(has(r.body, "id: 3\nevent: done"));
    auto whole = st.get(id); // завершённое задание: журнал целиком и закрытие
    CHECK(has(whole.body, "id: 1\nevent: tool"));
    CHECK(has(whole.body, "event: done"));
}

TEST(events_route_hides_foreign_non_chat_and_disabled) {
    Stand st;
    auto id = st.chat_job("u1");
    CHECK_EQ(st.get(id, "u2").status, 404);
    CHECK_EQ(st.get("00000000-0000-4000-8000-000000000000").status, 404);
    auto other = st.chat_job("u1", "parse_expense");
    CHECK_EQ(st.get(other).status, 404);
    Stand off(5, 3, /*enabled=*/false);
    CHECK_EQ(off.get(off.chat_job()).status, 404);
}

TEST(events_route_cancel_ends_stream_with_error) {
    Stand st;
    auto id = st.chat_job();
    std::thread canceller([&] {
        std::this_thread::sleep_for(100ms);
        st.state.jobs->cancel(id, "u1");
    });
    auto r = st.get(id);
    canceller.join();
    CHECK(has(r.body, "event: error\ndata: {\"failure\":\"cancelled\"}"));
}

TEST(events_route_limits_streams) {
    Stand st(/*total=*/1, /*per_user=*/1);
    auto a = st.chat_job("u1");
    auto b = st.chat_job("u1");
    Stand::Reply first;
    std::thread holder([&] { first = st.get(a); });
    std::this_thread::sleep_for(150ms); // первый поток открыт и молчит
    auto second = st.get(b);
    CHECK_EQ(second.status, 429);
    CHECK(has(second.body, "busy"));
    st.state.jobs->complete(a, jobs::Status::Done, json::object(), "");
    holder.join();
    CHECK_EQ(first.status, 200);
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(st.state.streams->open(), 0); // слот освобождён
    st.state.jobs->complete(b, jobs::Status::Done, json::object(), "");
    CHECK_EQ(st.get(b).status, 200); // и снова доступен
}

TEST(events_route_client_disconnect_releases_slot_and_keeps_job) {
    Stand st;
    auto id = st.chat_job();
    {
        httplib::Client cli("127.0.0.1", st.port);
        cli.set_read_timeout(5, 0);
        int chunks = 0;
        cli.Get("/events/" + id, {{"X-User", "u1"}}, [&](const char*, std::size_t) { return ++chunks < 2; }); // обрыв после пульсов
    }
    for (int i = 0; i < 40 && st.state.streams->open() > 0; ++i) std::this_thread::sleep_for(50ms);
    CHECK_EQ(st.state.streams->open(), 0);
    auto job = st.state.jobs->get(id, "u1");
    REQUIRE(job.has_value());
    CHECK(job->status == jobs::Status::Running); // обрыв клиента задание не трогает
}

TEST(events_route_window_closes_stream_and_resumes) {
    Stand st;
    st.state.stream_ping = 5s; // пульс не мешает: закрывает именно окно
    auto id = st.chat_job();
    st.state.jobs->append_event(id, "tool", {{"name", "a"}, {"outcome", "ok"}});
    auto t0 = std::chrono::steady_clock::now();
    auto r = st.get(id, "u1", "", "?window_ms=300");
    auto took = std::chrono::steady_clock::now() - t0;
    CHECK_EQ(r.status, 200);
    CHECK(has(r.body, "id: 1\nevent: tool"));
    CHECK(!has(r.body, "event: done"));        // задание не закончилось — поток закрыт окном
    CHECK(took < 2s);                          // не ждали пульса (5 с) и не висели
    CHECK(!has(r.body, ": ping"));
    for (int i = 0; i < 40 && st.state.streams->open() > 0; ++i) std::this_thread::sleep_for(25ms);
    CHECK_EQ(st.state.streams->open(), 0);    // слот свободен сразу, без ожидания пульса
    // Следующее окно продолжает с последнего id и получает остальное.
    st.state.jobs->append_event(id, "delta", {{"text", "хвост"}});
    st.state.jobs->complete(id, jobs::Status::Done, json{{"text", "хвост"}}, "");
    auto next = st.get(id, "u1", "1", "?window_ms=300");
    CHECK(has(next.body, "id: 2\nevent: delta"));
    CHECK(has(next.body, "event: done"));
}
