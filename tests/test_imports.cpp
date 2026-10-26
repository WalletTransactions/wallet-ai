// Импорт выписок: задания, приём, планировщик, категории, проверка и запись
// (openspec/changes/add-statement-import) — без Postgres (MemRepo) и без LLM.
#include <regex>

#include "ai.hpp"
#include "imports.hpp"
#include "mem_repo.hpp"
#include "statement/textutil.hpp"
#include "test_framework.hpp"

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr stmt::Epoch kDay = 86400;
constexpr stmt::Epoch kNow = 1792627200; // 2026-10-22 00:00:00 UTC

const char* kWallet = "11111111-1111-4111-8111-111111111111";
const char* kTaxi = "22222222-2222-4222-8222-222222222222";
const char* kCafe = "33333333-3333-4333-8333-333333333333";
const char* kHealth = "44444444-4444-4444-8444-444444444444";

json accounts() {
    return {{"accounts",
             {{{"uuid", kWallet}, {"kind", "wallet"}, {"name", "Тинькофф"}, {"iso_code", "RUB"}, {"symbol", "₽"},
               {"precision", "2"}},
              {{"uuid", kTaxi}, {"kind", "category"}, {"name", "Такси"}},
              {{"uuid", kCafe}, {"kind", "category"}, {"name", "Кафе"}},
              {{"uuid", kHealth}, {"kind", "category"}, {"name", "Здоровье"}}}}};
}

class FakeReports : public reports::ReportsApi {
public:
    int ops_status = 200;
    json ops = json::array();
    int ops_calls = 0;
    reports::Response get(const std::string& path, const std::string&) override {
        if (path.rfind("/api/reports/accounts", 0) == 0) return {200, accounts(), ""};
        ++ops_calls;
        if (ops_status != 200) return {ops_status, nullptr, "down"};
        return {200, {{"operations", ops}, {"has_more", false}, {"incomplete", false}}, ""};
    }
};

// «Модель»: разбирает строки «N| ДД.ММ.ГГГГ ОПИСАНИЕ -1 290,00» сама.
class FakeLlm : public llm::Client {
public:
    std::string version = "test";
    std::vector<llm::Status> rows_script; // статусы по очереди, дальше — Ok
    bool duplicate = false;
    bool skip_first = false;
    std::vector<std::vector<int>> rows_calls;
    std::string last_profile; // профиль из последнего extract_rows
    std::function<llm::ClassifyResult(const llm::ClassifyRequest&)> classify_fn;
    int classify_calls = 0;

    llm::Result extract_expense(const llm::ExpenseRequest&, const llm::CallOptions&) override { return {}; }
    const std::string& prompt_version() const override { return version; }

    llm::RowsResult extract_rows(const llm::RowsRequest& r, const llm::CallOptions&) override {
        llm::RowsResult out;
        out.status = llm::Status::Ok;
        last_profile = r.profile;
        if (!rows_script.empty()) {
            out.status = rows_script.front();
            rows_script.erase(rows_script.begin());
        }
        static const std::regex re(R"((\d+)\| (\d{2})\.(\d{2})\.(\d{4}) (.+?) (-?)([\d ]+,\d{2}))");
        std::vector<int> seen;
        for (const auto& line : stmt::split_lines(r.numbered_text)) {
            std::smatch m;
            int n = std::stoi(line.substr(0, line.find('|')));
            seen.push_back(n);
            if (!std::regex_search(line, m, re)) continue;
            std::string amount = m[7];
            amount.erase(std::remove(amount.begin(), amount.end(), ' '), amount.end());
            std::replace(amount.begin(), amount.end(), ',', '.');
            out.rows.push_back({{std::stoi(m[1])},
                                std::string(m[4]) + "-" + std::string(m[3]) + "-" + std::string(m[2]),
                                amount,
                                m[6] == "-" ? "expense" : "income",
                                "",
                                m[5]});
        }
        rows_calls.push_back(seen);
        if (skip_first && !out.rows.empty()) out.rows.erase(out.rows.begin());    // пропуск модели
        if (duplicate && !out.rows.empty()) out.rows.push_back(out.rows.front()); // одна строка дважды
        if (out.status != llm::Status::Ok) out.rows.clear();
        return out;
    }

    llm::ClassifyResult classify_rows(const llm::ClassifyRequest& r, const llm::CallOptions&) override {
        ++classify_calls;
        if (classify_fn) return classify_fn(r);
        llm::ClassifyResult out;
        out.status = llm::Status::Ok;
        for (std::size_t i = 0; i < r.patterns.size(); ++i) {
            std::string cat = r.patterns[i].find("YANDEX") != std::string::npos ? "Такси"
                              : r.patterns[i].find("APTEKA") != std::string::npos ? "Здоровье"
                                                                                  : "Кафе";
            double conf = r.patterns[i].find("APTEKA") != std::string::npos ? 0.5 : 0.9;
            out.items.push_back({static_cast<int>(i), cat, conf});
        }
        return out;
    }
};

struct World {
    MemRepo repo;
    FakeReports reports;
    FakeLlm llm;
    jobs::Store store{jobs::Limits{}};
    stmt::Epoch now = kNow;
    bool llm_ok = true;
    imports::Config cfg;
    std::unique_ptr<imports::Service> service;
    std::unique_ptr<imports::Runner> runner;
    std::vector<std::string> order; // import_id выполненных заданий по порядку

    explicit World(std::vector<stmt::Profile> profiles = {}, std::size_t chunk_chars = 1800) {
        cfg.chunk_chars = chunk_chars;
        cfg.min_chunk_chars = 40;
        service = std::make_unique<imports::Service>(repo, reports, std::move(profiles), cfg, [this] { return now; });
        runner = std::make_unique<imports::Runner>(repo, store, llm, *service, [this] { return llm_ok; },
                                                   llm::CallOptions{}, llm::CallOptions{});
        service->on_delete = [this](const std::string& id) { runner->cancel_import(id); };
    }

    // Один шаг: планирование и одно задание из очереди.
    bool step() {
        runner->tick();
        if (store.queued(jobs::Priority::Interactive) + store.queued(jobs::Priority::Background) == 0) return false;
        auto job = store.take();
        parse_expense::Outcome out = job->kind == "extract_rows"    ? runner->handle_extract(*job)
                                     : job->kind == "classify_rows" ? runner->handle_classify(*job)
                                                                    : parse_expense::Outcome{};
        order.push_back(job->input.value("import_id", job->kind));
        store.complete(job->id, out.status, out.result, out.failure);
        return true;
    }
    void drain(int max = 200) {
        for (int i = 0; i < max && step(); ++i) {
        }
        runner->tick();
    }
    imports::Reply create(const std::string& user, json body) {
        if (!body.contains("wallet_uuid")) body["wallet_uuid"] = kWallet;
        return service->create({user, "tok"}, body);
    }
    std::string create_ok(const std::string& user, const std::string& text) {
        auto r = create(user, {{"text", text}});
        REQUIRE(r.status == 202);
        return r.body["import_id"];
    }
    stmt::ImportRec imp(const std::string& id) { return *repo.get_import(id, ""); }
};

std::string statement(int n, const std::string& desc = "KOFE", int first_day = 1) {
    std::string s = "АО «Банк» Выписка за период с 01.09.2026 по 30.09.2026\n";
    for (int i = 0; i < n; ++i) {
        char line[96];
        std::snprintf(line, sizeof(line), "%02d.09.2026 %s %d -%d,00\n", first_day + i % 28, desc.c_str(), i,
                      100 + i);
        s += line;
    }
    return s + "Итого расходов: 99 999,00\n";
}

int group(const json& view, const char* g) { return view["groups"][g].get<int>(); }

} // namespace

TEST(jobs_internal_bypass_user_limits_and_yield) {
    jobs::Limits l;
    l.max_active_per_user = 1;
    jobs::Store s(l);
    for (int i = 0; i < 5; ++i) s.create_internal("u1", "extract_rows", {{"i", i}});
    // Импорт не съедает лимит трат: parse_expense принимается.
    auto c = s.create("u1", "parse_expense", jobs::Priority::Interactive, json::object(), "");
    CHECK(c.error == jobs::CreateError::None);
    CHECK_EQ(s.take()->kind, std::string("parse_expense")); // трата раньше оставшихся фрагментов
    CHECK_EQ(s.take()->kind, std::string("extract_rows"));
}

TEST(worker_dispatch_by_kind) {
    AppState state;
    state.jobs = std::make_unique<jobs::Store>(jobs::Limits{});
    int calls = 0;
    state.job_handlers["classify_rows"] = [&](const jobs::Job&) {
        ++calls;
        return parse_expense::Outcome{jobs::Status::Done, json::object(), "", {}};
    };
    jobs::Job job;
    job.kind = "classify_rows";
    CHECK(aiapi::run_job(state, job).status == jobs::Status::Done);
    CHECK_EQ(calls, 1);
    job.kind = "summarize";
    auto out = aiapi::run_job(state, job);
    CHECK(out.status == jobs::Status::Failed);
    CHECK_EQ(out.failure, std::string("internal"));
    CHECK(aiapi::status_json(llm::Config{}, llm::ProbeResult::Ok, true)["features"] ==
          json({"parse_expense", "statement_import"}));
    CHECK(aiapi::status_json(llm::Config{}, llm::ProbeResult::Ok, false)["features"] == json({"parse_expense"}));
    CHECK(aiapi::status_json(llm::Config{}, llm::ProbeResult::Unreachable, true)["features"] == json::array());
}

TEST(import_create_rejections) {
    World w;
    CHECK_EQ(w.create("u1", {{"text", "x"}, {"file", {{"name", "a.csv"}, {"content_base64", "eA"}}}}).body["error"],
             "one_source");
    CHECK_EQ(w.create("u1", json::object()).body["error"], "empty");
    CHECK_EQ(w.create("u1", {{"text", "  \n "}}).body["error"], "empty");
    CHECK_EQ(w.create("u1", {{"text", "x"}, {"wallet_uuid", kTaxi}}).body["error"], "unknown_wallet");
    CHECK_EQ(w.create("u1", {{"text", "x"}, {"profile", "nope"}}).body["error"], "unknown_profile");
    CHECK_EQ(w.create("u1", {{"file", {{"name", "a.xlsx"}, {"content_base64", "eA"}}}}).body["error"],
             "unsupported_file");
    World small;
    small.service = std::make_unique<imports::Service>(small.repo, small.reports, std::vector<stmt::Profile>{},
                                                       [] { imports::Config c; c.max_chars = 50; return c; }(),
                                                       [] { return kNow; });
    auto big = small.create("u1", {{"text", statement(5)}});
    CHECK_EQ(big.status, 413);
    World tiny({}, 60);
    tiny.service = std::make_unique<imports::Service>(tiny.repo, tiny.reports, std::vector<stmt::Profile>{},
                                                      [] { imports::Config c; c.chunk_chars = 60; c.min_chunk_chars = 40;
                                                           c.max_chunks = 3; return c; }(),
                                                      [] { return kNow; });
    CHECK_EQ(tiny.create("u1", {{"text", statement(20)}}).body["error"], "too_large"); // больше 80 фрагментов

    // Один импорт в работе на пользователя, AI_IMPORTS_MAX_ACTIVE всего.
    std::string id = w.create_ok("u1", statement(3));
    auto again = w.create("u1", {{"text", statement(3)}});
    CHECK_EQ(again.status, 429);
    CHECK_EQ(again.body["error"], "import_in_progress");
    CHECK_EQ(again.body["import_id"], id);
    w.create_ok("u2", statement(3));
    w.create_ok("u3", statement(3));
    CHECK_EQ(w.create("u4", {{"text", statement(3)}}).body["error"], "busy");
}

TEST(import_full_flow_to_ready) {
    World w({}, 120);
    std::string id = w.create_ok("u1", statement(6, "YANDEX*GO MOSCOW RUS") + "03.09.2026 APTEKA 36.6 -1 290,00\n");
    CHECK_EQ(w.imp(id).status, std::string("queued"));
    int total = w.imp(id).progress_total;
    CHECK(total > 1);
    w.step();
    auto mid = w.service->get({"u1", "t"}, id, false).body;
    CHECK_EQ(mid["status"], "extracting");
    CHECK_EQ(mid["progress"]["done"], 1);
    CHECK_EQ(mid["progress"]["total"], total);
    w.drain();
    auto v = w.service->get({"u1", "t"}, id, false).body;
    CHECK_EQ(v["status"], "ready");
    CHECK_EQ(group(v, "ready"), 6);           // YANDEX*GO → Такси (0.9)
    CHECK_EQ(group(v, "needs_category"), 1);  // APTEKA 0.5 → suggestion
    CHECK_EQ(group(v, "unparsed"), 0);        // «Итого» не в счёт
    for (const auto& r : v["rows"]) {
        if (r["pattern"] == "APTEKA 36.6") {
            CHECK(r["category_uuid"].is_null());
            CHECK_EQ(r["suggestion"], "Здоровье");
        }
    }
    CHECK_EQ(v["operations"].size(), 6u);
    auto op = v["operations"][0];
    CHECK_EQ(op["acc_fr_uuid"], kWallet);
    CHECK_EQ(op["acc_to_uuid"], kTaxi);
    CHECK_EQ(op["date"].get<std::string>().substr(10), " 12:00:00");
    CHECK_EQ(w.llm.classify_calls, 1);
    auto list = w.service->list({"u1", "t"}).body["imports"];
    REQUIRE(list.size() == 1u);
    CHECK_EQ(list[0]["counts"]["rows"], 7);
    CHECK_EQ(list[0]["counts"]["ready"], 6);
    CHECK_EQ(list[0]["counts"]["committed"], 0);
}

TEST(import_round_robin_and_resume) {
    World w({}, 60);
    std::string a = w.create_ok("ua", statement(8, "A"));
    w.now += 1;
    std::string b = w.create_ok("ub", statement(4, "B"));
    w.drain();
    // A1 B1 A2 B2 …: B не ждёт весь A.
    REQUIRE(w.order.size() >= 4u);
    CHECK_EQ(w.order[0], a);
    CHECK_EQ(w.order[1], b);
    CHECK_EQ(w.order[2], a);
    CHECK_EQ(w.order[3], b);

    // Рестарт посреди разбора: новый Runner, разобранные фрагменты не повторяются.
    World r({}, 120);
    std::string id = r.create_ok("u1", statement(8));
    r.step();
    auto calls_before = r.llm.rows_calls.size();
    r.runner = std::make_unique<imports::Runner>(r.repo, r.store, r.llm, *r.service, [] { return true; },
                                                 llm::CallOptions{}, llm::CallOptions{});
    // Задание первого раннера в очереди потеряно вместе с процессом.
    while (r.store.queued(jobs::Priority::Background) > 0) r.store.take();
    r.drain();
    CHECK_EQ(r.imp(id).status, std::string("ready"));
    CHECK_EQ(r.llm.rows_calls.size() - calls_before, r.repo.chunks(id).size() - 1);
}

TEST(import_pause_timeout_and_retry) {
    World w({}, 120);
    std::string id = w.create_ok("u1", statement(6));
    w.llm_ok = false;
    w.runner->tick();
    CHECK(w.imp(id).paused_since.has_value());
    CHECK_EQ(w.store.queued(jobs::Priority::Background), 0u);
    w.now += 59 * 60;
    w.runner->tick();
    CHECK_EQ(w.imp(id).status, std::string("queued"));
    // LM Studio перезапустили — разбор продолжается.
    w.llm_ok = true;
    w.step();
    CHECK(!w.imp(id).paused_since.has_value());
    CHECK_EQ(w.imp(id).status, std::string("extracting"));
    w.llm_ok = false;
    w.runner->tick();
    w.now += 61 * 60;
    w.runner->tick();
    CHECK_EQ(w.imp(id).status, std::string("failed"));
    CHECK_EQ(w.imp(id).failure, std::string("unavailable"));

    // Три таймаута фрагмента подряд — failed: timeout; retry — с первого неразобранного.
    World t({}, 120);
    std::string tid = t.create_ok("u1", statement(6));
    t.step();
    int done_before = t.imp(tid).progress_done;
    t.llm.rows_script = {llm::Status::Timeout, llm::Status::Timeout, llm::Status::Timeout};
    t.drain();
    CHECK_EQ(t.imp(tid).status, std::string("failed"));
    CHECK_EQ(t.imp(tid).failure, std::string("timeout"));
    CHECK_EQ(t.imp(tid).progress_done, done_before);
    CHECK_EQ(t.service->retry({"u1", "t"}, tid).status, 202);
    t.drain();
    CHECK_EQ(t.imp(tid).status, std::string("ready"));
    CHECK_EQ(t.service->retry({"u1", "t"}, tid).status, 409);
}

TEST(import_bad_response_split_and_collapse) {
    World w;
    std::string id = w.create_ok("u1", statement(4));
    w.llm.rows_script = {llm::Status::BadResponse};
    w.drain();
    REQUIRE(w.llm.rows_calls.size() == 3u); // целиком и две половины
    CHECK_EQ(w.repo.rows(id).size(), 4u);

    // Обе половины битые — строки уходят в «не разобрано», импорт доходит до ready.
    World g;
    std::string gid = g.create_ok("u1", statement(4));
    g.llm.rows_script = {llm::Status::BadResponse, llm::Status::BadResponse, llm::Status::BadResponse};
    g.drain();
    auto v = g.service->get({"u1", "t"}, gid, false).body;
    CHECK_EQ(v["status"], "ready");
    CHECK_EQ(group(v, "unparsed"), 4);
}

TEST(import_model_skips_and_duplicates) {
    World w;
    w.llm.duplicate = true;
    w.llm.skip_first = true;
    std::string id = w.create_ok("u1", statement(4));
    w.drain();
    auto v = w.service->get({"u1", "t"}, id, false).body;
    CHECK_EQ(v["rows"].size(), 3u);      // повтор схлопнут по lines
    CHECK_EQ(group(v, "unparsed"), 1);   // пропуск виден
    CHECK_EQ(v["unparsed"][0]["n"], 2);
}

TEST(import_delete_cancels_and_ttl) {
    World w({}, 120);
    std::string id = w.create_ok("u1", statement(6));
    w.runner->tick();
    CHECK_EQ(w.store.queued(jobs::Priority::Background), 1u);
    CHECK_EQ(w.service->remove({"u2", "t"}, id).status, 404); // чужой
    CHECK_EQ(w.service->remove({"u1", "t"}, id).status, 204);
    CHECK_EQ(w.store.queued(jobs::Priority::Background), 0u); // фрагменты не уйдут в LLM
    CHECK_EQ(w.service->get({"u1", "t"}, id, false).status, 404);

    // ready на 89-й день жив, на 91-й удалён.
    std::string r = w.create_ok("u1", statement(2));
    w.drain();
    w.now = kNow + 89 * kDay;
    w.runner->sweep();
    CHECK(w.repo.get_import(r, "").has_value());
    w.now = kNow + 91 * kDay;
    w.runner->sweep();
    CHECK(!w.repo.get_import(r, "").has_value());

    // done живёт 7 дней после записи, отпечатки остаются.
    w.now = kNow + 100 * kDay;
    std::string d = w.create_ok("u1", statement(2));
    w.drain();
    w.now += 30 * kDay;
    auto v = w.service->get({"u1", "t"}, d, false).body;
    json ids = json::array();
    for (const auto& row : v["rows"]) ids.push_back(row["row_id"]);
    auto after = w.service->committed({"u1", "t"}, d, {{"row_ids", ids}}).body;
    CHECK_EQ(after["status"], "done");
    CHECK_EQ(w.imp(d).expires_at, w.now + 7 * kDay);
    w.now += 7 * kDay;
    w.runner->sweep();
    CHECK(!w.repo.get_import(d, "").has_value());
    CHECK_EQ(w.repo.imported_set.size(), 2u);
}

TEST(import_rules_batches_and_classify_failure) {
    World w;
    w.repo.upsert_rule("u1", "SHOPAA", kCafe);
    std::string text = "Выписка\n";
    // Имена без цифр в конце: «SHOP12» по D12 — номер магазина сети «SHOP».
    for (int i = 0; i < 35; ++i) {
        std::string name = std::string("SHOP") + static_cast<char>('A' + i / 26) + static_cast<char>('A' + i % 26);
        text += "0" + std::to_string(1 + i % 9) + ".09.2026 " + name + " -10,00\n";
    }
    std::string id = w.create_ok("u1", text);
    while (w.imp(id).status != "classifying") w.step();
    CHECK_EQ(w.imp(id).progress_total, 2); // 34 pattern без правила → 2 пачки по 20
    for (const auto& r : w.repo.rows(id)) {
        if (r.pattern == "SHOPAA") CHECK_EQ(r.category_source, std::string("rule"));
    }
    w.drain();
    CHECK_EQ(w.llm.classify_calls, 2);

    // LM Studio выключили после извлечения — импорт всё равно ready, без категорий.
    World f;
    f.llm.classify_fn = [](const llm::ClassifyRequest&) {
        llm::ClassifyResult r;
        r.status = llm::Status::Unavailable;
        return r;
    };
    std::string fid = f.create_ok("u1", statement(3, "YANDEX*GO"));
    f.drain();
    auto v = f.service->get({"u1", "t"}, fid, false).body;
    CHECK_EQ(v["status"], "ready");
    CHECK_EQ(group(v, "needs_category"), 3);
}

TEST(import_reconcile_groups) {
    World w;
    std::string id = w.create_ok("u1", "12.09.2026 YANDEX*GO -450,00\n12.09.2026 KOFE -200,00\n"
                                        "12.09.2026 KOFE -200,00\n14.09.2026 SHOP -300,00\n"
                                        "15.09.2026 SALARY 50 000,00\n");
    w.drain();
    w.reports.ops = json::array({
        {{"operation_uuid", "op1"}, {"kind", "expense"}, {"wallet_uuid", kWallet}, {"amount", "450.00"},
         {"date", "2026-09-13 10:00:00"}},
        {{"operation_uuid", "op2"}, {"kind", "expense"}, {"wallet_uuid", kWallet}, {"amount", "200"},
         {"date", "2026-09-12 09:00:00"}},
        {{"operation_uuid", "op3"}, {"kind", "expense"}, {"wallet_uuid", kWallet}, {"amount", "300.00"},
         {"date", "2026-09-14 09:00:00"}},
        {{"operation_uuid", "op4"}, {"kind", "expense"}, {"wallet_uuid", kWallet}, {"amount", "300.00"},
         {"date", "2026-09-15 09:00:00"}},
        {{"operation_uuid", "op5"}, {"kind", "expense"}, {"wallet_uuid", "other"}, {"amount", "200.00"},
         {"date", "2026-09-12 09:00:00"}},
    });
    auto v = w.service->get({"u1", "t"}, id, false).body;
    CHECK_EQ(v["reconciled"], true);
    CHECK_EQ(group(v, "exists"), 2);             // такси ±1 день и один из двух кофе
    CHECK_EQ(group(v, "possible_duplicate"), 1); // два кандидата 300
    CHECK_EQ(group(v, "income_transfer"), 1);
    CHECK_EQ(group(v, "ready"), 1);              // второй кофе — к записи

    // Кэш минуту: повторный GET без похода в wallet-report.
    int calls = w.reports.ops_calls;
    w.service->get({"u1", "t"}, id, false);
    CHECK_EQ(w.reports.ops_calls, calls);
    w.reports.ops_status = 502;
    auto down = w.service->get({"u1", "t"}, id, true).body;
    CHECK_EQ(down["reconciled"], false);
    CHECK_EQ(group(down, "exists"), 2); // прошлая сверка осталась

    // Ранее импортированные строки.
    std::vector<std::string> fps;
    for (const auto& r : w.repo.rows(id)) fps.push_back(r.fingerprint);
    w.repo.add_imported("u1", fps);
    w.reports.ops_status = 200;
    auto again = w.service->get({"u1", "t"}, id, true).body;
    CHECK_EQ(group(again, "imported"), 4);
    CHECK_EQ(w.service->get({"u2", "t"}, id, false).status, 404);
}

TEST(import_patch_rows_and_manual_row) {
    World w;
    std::string id = w.create_ok("u1", "Дата операции Сумма Описание\n12.09.2026 YANDEX*GO -450,00\n"
                                        "13.09.2026 YANDEX*GO -460,00\n14.09.2026 APTEKA 36.6 -1 290,00\n"
                                        "15.09.2026 LOST -99,00\n");
    w.llm.classify_fn = [](const llm::ClassifyRequest& r) {
        llm::ClassifyResult out;
        out.status = llm::Status::Ok;
        for (std::size_t i = 0; i < r.patterns.size(); ++i) out.items.push_back({static_cast<int>(i), "", 0.1});
        return out;
    };
    // Модель «потеряла» строку 5 и ошиблась в сумме строки 4.
    w.drain();
    auto rows = w.repo.rows(id);
    REQUIRE(rows.size() == 4u);
    auto& lost = w.repo.row_map[id];
    lost.erase(std::remove_if(lost.begin(), lost.end(), [](const stmt::RowRec& r) { return r.pattern == "LOST"; }),
               lost.end());
    for (auto& r : lost) {
        if (r.pattern == "APTEKA 36.6") {
            r.amount = "1209.00";
            r.check = "amount_not_in_text";
        }
    }
    auto v = w.service->get({"u1", "t"}, id, false).body;
    CHECK_EQ(group(v, "needs_category"), 2);
    CHECK_EQ(group(v, "check"), 1);
    CHECK_EQ(group(v, "unparsed"), 2); // LOST и строка с непроверенной суммой

    std::string taxi_row;
    std::string apteka_row;
    for (const auto& r : v["rows"]) {
        if (r["pattern"] == "YANDEX*GO" && taxi_row.empty()) taxi_row = r["row_id"];
        if (r["pattern"] == "APTEKA 36.6") apteka_row = r["row_id"];
    }
    // Одна правка категории — правило и соседняя строка того же продавца.
    auto p = w.service->patch_rows({"u1", "t"}, id, {{"rows", {{{"row_id", taxi_row}, {"category_uuid", kTaxi}}}}});
    CHECK_EQ(p.status, 200);
    CHECK_EQ(group(p.body, "ready"), 2);
    CHECK_EQ(w.repo.rules("u1")["YANDEX*GO"], std::string(kTaxi));

    // Исправление суммы снимает check и учит свой формат.
    auto fix = w.service->patch_rows({"u1", "t"}, id, {{"rows", {{{"row_id", apteka_row}, {"amount", "1290.00"}}}}});
    CHECK_EQ(group(fix.body, "check"), 0);
    CHECK_EQ(group(fix.body, "needs_category"), 1);
    CHECK_EQ(w.repo.profile_map.size(), 1u);
    CHECK_EQ(w.service->patch_rows({"u1", "t"}, id, {{"rows", {{{"row_id", apteka_row}, {"amount", "0"}}}}}).status,
             400);
    CHECK_EQ(w.service->patch_rows({"u1", "t"}, id, {{"rows", {{{"row_id", apteka_row}, {"date", "2030-01-01"}}}}})
                 .status,
             400);

    // Строка из «не разобрано».
    auto added = w.service->add_row({"u1", "t"}, id, {{"line", 5}, {"amount", "99.00"}, {"date", "2026-09-15"},
                                                      {"category_uuid", kCafe}, {"description", "LOST"}});
    CHECK_EQ(added.status, 201);
    CHECK_EQ(group(added.body, "unparsed"), 0);
    bool manual = false;
    for (const auto& r : added.body["rows"]) manual = manual || r["source"] == "manual";
    CHECK(manual);

    // Включить дубль.
    w.reports.ops = json::array({{{"operation_uuid", "op1"}, {"kind", "expense"}, {"wallet_uuid", kWallet},
                                  {"amount", "450.00"}, {"date", "2026-09-12 10:00:00"}}});
    auto rec = w.service->get({"u1", "t"}, id, true).body;
    CHECK_EQ(group(rec, "exists"), 1);
    std::string dup;
    for (const auto& r : rec["rows"]) {
        if (r["group"] == "exists") dup = r["row_id"];
    }
    auto inc = w.service->patch_rows({"u1", "t"}, id, {{"rows", {{{"row_id", dup}, {"decision", "include"}}}}});
    CHECK_EQ(group(inc.body, "exists"), 0);

    // Запись частями: done — только когда закрыты все строки.
    auto part = w.service->committed({"u1", "t"}, id, {{"row_ids", {dup}}}).body;
    CHECK_EQ(part["status"], "ready");
    CHECK_EQ(w.repo.imported_set.size(), 1u);
}

TEST(import_profiles_detect_and_manage) {
    auto tbank = stmt::parse_profile("# version: t-1\nid: tbank\nname: ТБанк\n[signature]\nАО «ТБанк»\n"
                                     "[hints]\nПоследняя колонка — остаток.\n[check]\nexpense_word: Списание\n");
    World w({tbank});
    auto auto_id = w.create_ok("u1", "АО «ТБанк»\n12.09.2026 KOFE -200,00\n");
    auto v = w.service->get({"u1", "t"}, auto_id, false).body;
    CHECK_EQ(v["profile"]["id"], "tbank");
    CHECK_EQ(v["profile"]["source"], "auto");
    w.service->remove({"u1", "t"}, auto_id);

    auto none_id = w.create_ok("u1", "Другой банк\n12.09.2026 KOFE -200,00\n");
    CHECK_EQ(w.service->get({"u1", "t"}, none_id, false).body["profile_detect"], "none");
    w.service->remove({"u1", "t"}, none_id);

    auto manual = w.create("u1", {{"text", "12.09.2026 KOFE -200,00\n"}, {"profile", "tbank"}});
    CHECK_EQ(w.service->get({"u1", "t"}, manual.body["import_id"], false).body["profile"]["source"], "manual");
    w.service->remove({"u1", "t"}, manual.body["import_id"]);

    // Свой формат: отпечаток заголовка, примеры из правок, затем автоопределение.
    std::string header = "Дата операции;Сумма;Описание\n";
    std::string first = w.create_ok("u1", header + "12.09.2026 KOFE -200,00\n");
    w.drain();
    auto rows = w.repo.rows(first);
    w.service->patch_rows({"u1", "t"}, first, {{"rows", {{{"row_id", rows[0].id}, {"amount", "201"}}}}});
    REQUIRE(w.repo.profile_map.size() == 1u);
    std::string own = w.repo.profile_map.begin()->first;
    auto next = w.create_ok("u2", header + "01.10.2026 X -1,00\n"); // другой пользователь — не его формат
    CHECK(w.service->get({"u2", "t"}, next, false).body["profile"].is_null());
    w.service->remove({"u1", "t"}, first);
    auto second = w.create_ok("u1", header + "13.10.2026 KOFE -300,00\n");
    auto sv = w.service->get({"u1", "t"}, second, false).body;
    CHECK_EQ(sv["profile"]["id"], own);
    CHECK(w.service->profile_prompt_for(w.imp(second)).find("KOFE") != std::string::npos);

    auto list = w.service->profiles({"u1", "t"}).body["profiles"];
    CHECK_EQ(list.size(), 2u);
    CHECK_EQ(list[1]["examples"], 1);
    CHECK_EQ(w.service->patch_profile({"u1", "t"}, "tbank", {{"name", "x"}}).status, 403);
    CHECK_EQ(w.service->patch_profile({"u2", "t"}, own, {{"name", "x"}}).status, 404);
    CHECK_EQ(w.service->patch_profile({"u1", "t"}, own, {{"name", "CSV из банка"}}).body["name"], "CSV из банка");
    CHECK_EQ(w.service->delete_profile({"u1", "t"}, own).status, 204);
    CHECK(w.service->get({"u1", "t"}, second, false).body["profile"].is_null());
}

TEST(import_profile_in_prompt_and_check) {
    // Остаток против суммы решает подсказка профиля в промпте модели; код
    // проверяет, что она туда дошла, и что знак из [check] перекрывает dir.
    auto prof = stmt::parse_profile("# version: b-1\nid: bank\nname: Банк\n[signature]\nАО «Банк»\n"
                                    "[hints]\nПоследняя колонка — остаток, не сумма.\n[check]\nexpense_word: Списание\n");
    World w({prof});
    auto id = w.create_ok("u1", "АО «Банк»\n12.09.2026 Списание KOFE 200,00\n13.09.2026 Зачисление ЗП 5 000,00\n");
    w.drain();
    CHECK(w.llm.last_profile.find("остаток") != std::string::npos);
    auto rows = w.repo.rows(id);
    REQUIRE(rows.size() == 2u);
    CHECK_EQ(rows[0].dir, std::string("expense")); // FakeLlm без минуса вернул income
    CHECK_EQ(rows[1].dir, std::string("income"));
}

TEST(import_csv_by_columns_without_llm) {
    auto prof = stmt::parse_profile("# version: c-1\nid: bankcsv\nname: CSV\n[signature]\ndate;type;desc;amount\n"
                                    "[columns]\ndelimiter: ;\ndate: Date | DD.MM.YYYY\namount: Amount\n"
                                    "description: Desc\ntype: Type\n[types]\nPay: expense\nSwap: transfer\n");
    std::string csv = "Date;Type;Desc;Amount\n12.09.2026;Pay;YANDEX*GO;-450\n13.09.2026;Swap;To savings;-1000\n"
                      "14.09.2026;Pay;KOFE;-200.5\n";
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for (std::size_t i = 0; i < csv.size(); i += 3) {
        unsigned n = static_cast<unsigned char>(csv[i]) << 16;
        if (i + 1 < csv.size()) n |= static_cast<unsigned char>(csv[i + 1]) << 8;
        if (i + 2 < csv.size()) n |= static_cast<unsigned char>(csv[i + 2]);
        enc += a[(n >> 18) & 63];
        enc += a[(n >> 12) & 63];
        enc += i + 1 < csv.size() ? a[(n >> 6) & 63] : '=';
        enc += i + 2 < csv.size() ? a[n & 63] : '=';
    }
    json file = {{"file", {{"name", "s.csv"}, {"content_base64", enc}}}};

    // LLM недоступна: разбор по колонкам всё равно доходит до ready, строки без категорий.
    World w({prof});
    w.llm_ok = false;
    auto r = w.create("u1", file);
    REQUIRE(r.status == 202);
    std::string id = r.body["import_id"];
    CHECK_EQ(w.repo.chunks(id).size(), 0u);
    w.drain();
    CHECK(w.llm.rows_calls.empty());
    auto v = w.service->get({"u1", "t"}, id, false).body;
    CHECK_EQ(v["status"], "ready");
    CHECK_EQ(v["profile"]["id"], "bankcsv");
    auto rows = w.repo.rows(id);
    REQUIRE(rows.size() == 3u);
    CHECK_EQ(rows[1].dir, std::string("transfer"));
    CHECK_EQ(rows[2].amount, std::string("200.50"));
    for (const auto& row : rows) CHECK_EQ(row.check, std::string(""));
    w.service->remove({"u1", "t"}, id);

    // LLM есть: заданий extract_rows нет, только разметка категорий.
    World w2({prof});
    auto id2 = w2.create("u1", file).body["import_id"].get<std::string>();
    w2.drain();
    CHECK(w2.llm.rows_calls.empty());
    CHECK_EQ(w2.llm.classify_calls, 1);
    CHECK_EQ(w2.service->get({"u1", "t"}, id2, false).body["status"], "ready");
}

TEST(import_without_operations_fails_fast) {
    World w;
    // Текст без единой строки с суммой: импорт сразу failed, текст не хранится.
    auto r = w.create("u1", {{"text", "Уважаемый клиент!\nВаш договор 12345 от 01.09.2026\nСпасибо"}});
    CHECK_EQ(r.status, 202);
    CHECK_EQ(r.body["status"], "failed");
    CHECK_EQ(r.body["failure"], "no_operations");
    std::string id = r.body["import_id"];
    CHECK(w.imp(id).source_text.empty());
    CHECK_EQ(w.service->retry({"u1", "t"}, id).status, 409);
    w.drain();
    CHECK(w.llm.rows_calls.empty());

    // CSV с профилем колонок, где ни одна строка не разобралась.
    auto prof = stmt::parse_profile("# version: c-1\nid: bankcsv\nname: CSV\n[signature]\ndate;desc;amount\n"
                                    "[columns]\ndelimiter: ;\ndate: Date | DD.MM.YYYY\namount: Amount\n"
                                    "description: Desc\n");
    World w2({prof});
    std::string csv = "Date;Desc;Amount\nxx;Итого;\n";
    std::string enc;
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < csv.size(); i += 3) {
        unsigned n = static_cast<unsigned char>(csv[i]) << 16;
        if (i + 1 < csv.size()) n |= static_cast<unsigned char>(csv[i + 1]) << 8;
        if (i + 2 < csv.size()) n |= static_cast<unsigned char>(csv[i + 2]);
        enc += a[(n >> 18) & 63];
        enc += a[(n >> 12) & 63];
        enc += i + 1 < csv.size() ? a[(n >> 6) & 63] : '=';
        enc += i + 2 < csv.size() ? a[n & 63] : '=';
    }
    auto r2 = w2.create("u1", {{"file", {{"name", "s.csv"}, {"content_base64", enc}}}});
    CHECK_EQ(r2.body["failure"], "no_operations");
}

TEST(import_pdf_scan_fails_fast) {
    World w;
    // Пустой PDF-скан: импорт сразу failed: no_text.
    std::string pdf = "%PDF-1.4\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
                      "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
                      "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >> endobj\n"
                      "trailer << /Root 1 0 R >>\n%%EOF\n";
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for (std::size_t i = 0; i < pdf.size(); i += 3) {
        unsigned n = static_cast<unsigned char>(pdf[i]) << 16;
        if (i + 1 < pdf.size()) n |= static_cast<unsigned char>(pdf[i + 1]) << 8;
        if (i + 2 < pdf.size()) n |= static_cast<unsigned char>(pdf[i + 2]);
        enc += a[(n >> 18) & 63];
        enc += a[(n >> 12) & 63];
        enc += i + 1 < pdf.size() ? a[(n >> 6) & 63] : '=';
        enc += i + 2 < pdf.size() ? a[n & 63] : '=';
    }
    auto r = w.create("u1", {{"file", {{"name", "scan.pdf"}, {"content_base64", enc}}}});
    CHECK_EQ(r.status, 202);
    CHECK_EQ(r.body["status"], "failed");
    CHECK_EQ(r.body["failure"], "no_text");
}
