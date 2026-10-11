#include "jobs.hpp"

#include <thread>

#include "test_framework.hpp"

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

// Ручные часы: TTL и лимит в минуту проверяются без ожидания.
struct FakeClock {
    jobs::Clock::time_point t = jobs::Clock::time_point{} + 1000h;
    jobs::Store::NowFn fn() {
        return [this] { return t; };
    }
};

jobs::Limits limits() {
    jobs::Limits l;
    l.max_queued_interactive = 3;
    l.max_active_per_user = 2;
    l.max_per_minute = 4;
    l.ttl = 600s;
    return l;
}

} // namespace

TEST(jobs_interactive_before_background) {
    FakeClock clock;
    jobs::Store s(limits(), clock.fn());
    auto bg = s.create("u1", "classify_rows", jobs::Priority::Background, json{{"i", 1}}, "");
    auto fg = s.create("u2", "parse_expense", jobs::Priority::Interactive, json{{"i", 2}}, "");
    auto first = s.take();
    REQUIRE(first.has_value());
    CHECK_EQ(first->id, fg.job.id);
    CHECK(first->status == jobs::Status::Running);
    CHECK_EQ(first->input["i"].get<int>(), 2);
    auto second = s.take();
    CHECK_EQ(second->id, bg.job.id);
}

TEST(jobs_owner_only_and_input_hidden) {
    FakeClock clock;
    jobs::Store s(limits(), clock.fn());
    auto c = s.create("u1", "parse_expense", jobs::Priority::Interactive, json{{"text", "секрет"}}, "");
    CHECK(!s.get(c.job.id, "u2").has_value()); // чужое — как будто нет
    CHECK(!s.cancel(c.job.id, "u2"));
    auto mine = s.get(c.job.id, "u1");
    REQUIRE(mine.has_value());
    CHECK(mine->input.is_null()); // вход клиенту не отдаётся
    CHECK(!s.get("00000000-0000-4000-8000-000000000000", "u1").has_value());
}

TEST(jobs_complete_drops_input_and_keeps_result_until_ttl) {
    FakeClock clock;
    jobs::Store s(limits(), clock.fn());
    auto c = s.create("u1", "parse_expense", jobs::Priority::Interactive, json{{"text", "x"}}, "");
    auto job = s.take();
    s.complete(job->id, jobs::Status::Done, json{{"outcome", "draft"}}, "");
    auto done = s.get(c.job.id, "u1");
    REQUIRE(done.has_value());
    CHECK(done->status == jobs::Status::Done);
    CHECK_EQ(done->result["outcome"].get<std::string>(), std::string("draft"));

    clock.t += 599s;
    s.sweep();
    CHECK(s.get(c.job.id, "u1").has_value());
    clock.t += 1s;
    s.sweep();
    CHECK(!s.get(c.job.id, "u1").has_value()); // истекло
}

TEST(jobs_idempotency_key_returns_same_job) {
    FakeClock clock;
    jobs::Store s(limits(), clock.fn());
    auto a = s.create("u1", "parse_expense", jobs::Priority::Interactive, json::object(), "tg:1:100");
    auto b = s.create("u1", "parse_expense", jobs::Priority::Interactive, json::object(), "tg:1:100");
    CHECK_EQ(a.job.id, b.job.id);
    CHECK(b.reused);
    CHECK_EQ(s.queued(jobs::Priority::Interactive), 1u); // второй раз в очередь не встал
    auto other_user = s.create("u2", "parse_expense", jobs::Priority::Interactive, json::object(), "tg:1:100");
    CHECK(other_user.job.id != a.job.id); // ключ — в пределах пользователя
}

TEST(jobs_limits) {
    FakeClock clock;
    jobs::Store s(limits(), clock.fn());
    CHECK(s.create("u1", "k", jobs::Priority::Interactive, {}, "").error == jobs::CreateError::None);
    CHECK(s.create("u1", "k", jobs::Priority::Interactive, {}, "").error == jobs::CreateError::None);
    // Третье незавершённое у того же пользователя.
    CHECK(s.create("u1", "k", jobs::Priority::Interactive, {}, "").error == jobs::CreateError::TooManyActive);
    CHECK(s.create("u2", "k", jobs::Priority::Interactive, {}, "").error == jobs::CreateError::None);
    // Очередь интерактивных полна (3).
    CHECK(s.create("u3", "k", jobs::Priority::Interactive, {}, "").error == jobs::CreateError::Busy);

    // Лимит в минуту: 4 созданных за 60 с.
    jobs::Store r(limits(), clock.fn());
    for (int i = 0; i < 4; ++i) {
        auto c = r.create("u1", "k", jobs::Priority::Background, {}, "");
        REQUIRE(c.error == jobs::CreateError::None);
        auto j = r.take();
        r.complete(j->id, jobs::Status::Done, {}, "");
    }
    CHECK(r.create("u1", "k", jobs::Priority::Background, {}, "").error == jobs::CreateError::RateLimited);
    clock.t += 61s;
    CHECK(r.create("u1", "k", jobs::Priority::Background, {}, "").error == jobs::CreateError::None);
}

TEST(jobs_cancel_queued_and_running) {
    FakeClock clock;
    jobs::Store s(limits(), clock.fn());
    auto q = s.create("u1", "k", jobs::Priority::Interactive, {}, "");
    CHECK(s.cancel(q.job.id, "u1"));
    CHECK(s.get(q.job.id, "u1")->status == jobs::Status::Cancelled);
    CHECK_EQ(s.queued(jobs::Priority::Interactive), 0u);
    CHECK(!s.cancel(q.job.id, "u1")); // уже завершено

    auto r = s.create("u1", "k", jobs::Priority::Interactive, {}, "");
    auto running = s.take();
    CHECK(s.cancel(r.job.id, "u1"));
    CHECK(s.get(r.job.id, "u1")->status == jobs::Status::Cancelled); // сразу, не по окончании генерации
    s.complete(running->id, jobs::Status::Done, json{{"outcome", "draft"}}, "");
    auto after = s.get(r.job.id, "u1");
    CHECK(after->status == jobs::Status::Cancelled); // результат выброшен
    CHECK(after->result.is_null());
}

TEST(jobs_long_poll_returns_on_completion_and_on_timeout) {
    jobs::Store s(limits());
    auto c = s.create("u1", "k", jobs::Priority::Interactive, {}, "");
    auto job = s.take();

    // Ожидание истекает — снимок как есть.
    auto t0 = jobs::Clock::now();
    auto still = s.get(c.job.id, "u1", 100ms);
    CHECK(still->status == jobs::Status::Running);
    CHECK(jobs::Clock::now() - t0 >= 90ms);

    // Завершение во время ожидания — возврат сразу, не по таймауту.
    std::thread worker([&] {
        std::this_thread::sleep_for(50ms);
        s.complete(job->id, jobs::Status::Done, json{{"outcome", "draft"}}, "");
    });
    t0 = jobs::Clock::now();
    auto done = s.get(c.job.id, "u1", 5000ms);
    worker.join();
    CHECK(done->status == jobs::Status::Done);
    CHECK(jobs::Clock::now() - t0 < 2000ms);
}

TEST(jobs_stop_unblocks_take) {
    jobs::Store s(limits());
    std::thread t([&] { CHECK(!s.take().has_value()); });
    std::this_thread::sleep_for(20ms);
    s.stop();
    t.join();
}
