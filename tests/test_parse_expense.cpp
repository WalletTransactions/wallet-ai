#include "parse_expense.hpp"

#include "test_framework.hpp"

using nlohmann::json;

namespace {

// LLM, которая отвечает заданным Result и запоминает запрос.
class FakeLlm : public llm::Client {
public:
    llm::Result next;
    llm::ExpenseRequest last;
    llm::CallOptions last_opts;
    int calls = 0;
    std::string version = "test-v1";

    llm::Result extract_expense(const llm::ExpenseRequest& r, const llm::CallOptions& opts) override {
        ++calls;
        last = r;
        last_opts = opts;
        return next;
    }
    const std::string& prompt_version() const override { return version; }
    llm::RowsResult extract_rows(const llm::RowsRequest&, const llm::CallOptions&) override { return {}; }
    llm::ClassifyResult classify_rows(const llm::ClassifyRequest&, const llm::CallOptions&) override { return {}; }
};

json accounts_body() {
    return json::parse(R"({"accounts":[
        {"uuid":"w1","kind":"wallet","name":"Тинькофф","iso_code":"RUB","symbol":"₽","precision":"2"},
        {"uuid":"w2","kind":"wallet","name":"Revolut","iso_code":"EUR","symbol":"€","precision":"2"},
        {"uuid":"c1","kind":"category","name":"Кафе"},
        {"uuid":"c2","kind":"category","name":"Такси"},
        {"uuid":"t1","kind":"category","name":"Transfer","system_kind":1}
    ]})");
}

json client_input(const std::string& text) {
    return {{"text", text}, {"now", {{"date", "2026-10-11"}, {"time", "14:05:00"}}}};
}

llm::Result ok(const std::string& amount, const std::string& category, const std::string& wallet,
               const std::string& date = "") {
    llm::Result r;
    r.status = llm::Status::Ok;
    r.extraction.amount = amount;
    r.extraction.category = category;
    r.extraction.wallet = wallet;
    r.extraction.date = date;
    r.extraction.confidence = 0.9;
    return r;
}

} // namespace

TEST(validate_input_rules) {
    CHECK(parse_expense::validate_input(client_input("кофе 200")).empty());
    CHECK(!parse_expense::validate_input(json::array()).empty());
    CHECK(!parse_expense::validate_input(client_input("")).empty());
    CHECK(!parse_expense::validate_input(client_input(std::string(501, 'x'))).empty());
    json no_now = {{"text", "кофе 200"}};
    CHECK(!parse_expense::validate_input(no_now).empty());
    json bad_date = client_input("кофе 200");
    bad_date["now"]["date"] = "11.10.2026";
    CHECK(!parse_expense::validate_input(bad_date).empty());
    json bad_time = client_input("кофе 200");
    bad_time["now"]["time"] = "14:05";
    CHECK(!parse_expense::validate_input(bad_time).empty());
}

TEST(run_draft_with_resolution) {
    FakeLlm llm;
    llm.next = ok("450", "Такси", "Тинькофф", "2026-10-10");
    auto input = parse_expense::make_job_input(client_input("вчера такси 450 с тинькоффа"), accounts_body());
    llm::CallOptions opts;
    opts.reasoning_effort = "low";
    auto out = parse_expense::run(input, llm, opts);

    CHECK(out.status == jobs::Status::Done);
    CHECK_EQ(out.result["outcome"].get<std::string>(), std::string("draft"));
    CHECK_EQ(out.result["expense"]["amount"].get<std::string>(), std::string("450"));
    CHECK_EQ(out.result["expense"]["datetime"].get<std::string>(), std::string("2026-10-10 12:00:00"));
    CHECK_EQ(out.result["resolution"]["category"]["kind"].get<std::string>(), std::string("found"));
    CHECK_EQ(out.result["resolution"]["category"]["found"]["uuid"].get<std::string>(), std::string("c2"));
    CHECK_EQ(out.result["resolution"]["wallet"]["found"]["uuid"].get<std::string>(), std::string("w1"));
    CHECK_EQ(out.result["prompt_version"].get<std::string>(), std::string("test-v1"));

    // В модель ушли имена пользователя без системных категорий.
    CHECK_EQ(llm.last.today, std::string("2026-10-11"));
    CHECK_EQ(llm.last.categories.size(), 2u);
    CHECK_EQ(llm.last.wallets.size(), 2u);
    CHECK_EQ(llm.last_opts.reasoning_effort, std::string("low"));
}

TEST(run_wallet_by_currency_and_category_ask) {
    FakeLlm llm;
    llm.next = ok("12", "", "");
    llm.next.extraction.currency = "EUR";
    auto out = parse_expense::run(parse_expense::make_job_input(client_input("12 евро обед"), accounts_body()), llm,
                                  llm::CallOptions{});
    REQUIRE(out.result["outcome"] == "draft");
    CHECK_EQ(out.result["resolution"]["wallet"]["found"]["uuid"].get<std::string>(), std::string("w2"));
    CHECK(out.result["resolution"]["wallet"]["by_currency"].get<bool>());
    CHECK_EQ(out.result["resolution"]["category"]["kind"].get<std::string>(), std::string("ask"));
    CHECK_EQ(out.result["resolution"]["category"]["why"].get<std::string>(), std::string("missing"));
}

TEST(run_rejected_when_amount_not_in_text) {
    FakeLlm llm;
    llm.next = ok("650", "Такси", "");
    auto out = parse_expense::run(parse_expense::make_job_input(client_input("200 и 450 такси"), accounts_body()),
                                  llm, llm::CallOptions{});
    CHECK(out.status == jobs::Status::Done);
    CHECK_EQ(out.result["outcome"].get<std::string>(), std::string("rejected"));
    CHECK_EQ(out.result["reasons"][0].get<std::string>(), std::string("amount_not_in_text"));
}

TEST(run_not_expense_and_failures) {
    FakeLlm llm;
    llm.next.status = llm::Status::NotExpense;
    auto input = parse_expense::make_job_input(client_input("привет"), accounts_body());
    auto ne = parse_expense::run(input, llm, llm::CallOptions{});
    CHECK(ne.status == jobs::Status::Done);
    CHECK_EQ(ne.result["outcome"].get<std::string>(), std::string("not_expense"));

    llm.next.status = llm::Status::Timeout;
    auto t = parse_expense::run(input, llm, llm::CallOptions{});
    CHECK(t.status == jobs::Status::Failed);
    CHECK_EQ(t.failure, std::string("timeout"));

    llm.next.status = llm::Status::ModelNotLoaded;
    CHECK_EQ(parse_expense::run(input, llm, llm::CallOptions{}).failure, std::string("unavailable"));
    llm.next.status = llm::Status::BadResponse;
    CHECK_EQ(parse_expense::run(input, llm, llm::CallOptions{}).failure, std::string("bad_response"));
}

TEST(run_log_has_no_text_or_amounts) {
    FakeLlm llm;
    llm.next = ok("450", "Такси", "Тинькофф");
    auto out = parse_expense::run(parse_expense::make_job_input(client_input("такси 450 секретно"), accounts_body()),
                                  llm, llm::CallOptions{});
    std::string log = out.log.dump();
    CHECK(log.find("секретно") == std::string::npos);
    CHECK(log.find("450") == std::string::npos);
    CHECK_EQ(out.log["outcome"].get<std::string>(), std::string("draft"));
}
