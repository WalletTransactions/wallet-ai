#include "expense_check.hpp"

#include <algorithm>

#include "test_framework.hpp"

namespace {

parser::Names names() {
    parser::Names n;
    n.categories = {"Кафе", "Продукты", "Такси"};
    n.wallets = {"Тинькофф", "Revolut"};
    n.currencies = {{"RUB", "₽"}, {"EUR", "€"}};
    return n;
}

const parser::Now kNow{"2026-10-11", "14:05:00"};

llm::Extraction ex(const std::string& amount, const std::string& date = "", const std::string& currency = "") {
    llm::Extraction e;
    e.amount = amount;
    e.date = date;
    e.currency = currency;
    e.category = "Кафе";
    e.comment = "кофе";
    e.confidence = 0.9;
    return e;
}

bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

} // namespace

TEST(check_amount_must_be_in_text) {
    auto ok = expense_check::check(ex("1200,50"), "1 200,50 продукты", names(), kNow);
    REQUIRE(ok.ok);
    CHECK_EQ(ok.expense.amount, std::string("1200.5"));
    CHECK_EQ(ok.expense.fraction_digits, 1);

    CHECK(expense_check::check(ex("12.50"), "обед 12.5 евро", names(), kNow).ok); // 12.50 ↔ 12.5
    CHECK(expense_check::check(ex("12"), "$12 кофе", names(), kNow).ok);
    CHECK(expense_check::check(ex("450"), "такси 450, вчера", names(), kNow).ok);

    auto summed = expense_check::check(ex("650"), "200 и 450 такси", names(), kNow);
    CHECK(!summed.ok);
    CHECK(has(summed.notes, "amount_not_in_text"));

    auto invented = expense_check::check(ex("1000000"), "кофе двести", names(), kNow);
    CHECK(has(invented.notes, "amount_not_in_text"));
}

TEST(check_bad_and_zero_amount) {
    CHECK(has(expense_check::check(ex("0"), "кофе 0", names(), kNow).notes, "zero_amount"));
    CHECK(has(expense_check::check(ex("abc"), "кофе", names(), kNow).notes, "bad_amount"));
    CHECK(has(expense_check::check(ex(""), "кофе", names(), kNow).notes, "bad_amount"));
}

TEST(check_dates) {
    auto today = expense_check::check(ex("200"), "кофе 200", names(), kNow);
    REQUIRE(today.ok);
    CHECK_EQ(today.expense.datetime, std::string("2026-10-11 14:05:00")); // сегодня — текущее время

    auto yesterday = expense_check::check(ex("200", "2026-10-10"), "вчера кофе 200", names(), kNow);
    CHECK_EQ(yesterday.expense.datetime, std::string("2026-10-10 12:00:00")); // прошлый день — полдень

    CHECK(has(expense_check::check(ex("200", "2026-10-12"), "кофе 200", names(), kNow).notes, "future_date"));
    CHECK(has(expense_check::check(ex("200", "2426-09-24"), "кофе 200", names(), kNow).notes, "future_date"));
    CHECK(has(expense_check::check(ex("200", "2025-10-09"), "кофе 200", names(), kNow).notes, "too_old"));
    CHECK(expense_check::check(ex("200", "2025-10-11"), "кофе 200", names(), kNow).ok); // ровно 365 дней
    CHECK(has(expense_check::check(ex("200", "2026-02-30"), "кофе 200", names(), kNow).notes, "bad_date"));
}

TEST(check_currency_only_known) {
    auto eur = expense_check::check(ex("12", "", "EUR"), "12 евро обед", names(), kNow);
    CHECK_EQ(eur.expense.currency_hint, std::string("EUR"));
    auto usd = expense_check::check(ex("12", "", "USD"), "12 долларов", names(), kNow);
    CHECK_EQ(usd.expense.currency_hint, std::string("USD")); // встроенный ISO
    auto junk = expense_check::check(ex("12", "", "XYZ"), "12 кофе", names(), kNow);
    REQUIRE(junk.ok);
    CHECK(junk.expense.currency_hint.empty());
    CHECK(has(junk.notes, "currency_dropped"));
}

TEST(check_comment_truncated) {
    auto e = ex("200");
    e.comment = std::string(800, 'x');
    auto r = expense_check::check(e, "кофе 200", names(), kNow);
    REQUIRE(r.ok);
    CHECK(r.expense.comment.size() <= expense_check::kMaxCommentBytes + 3); // + «…» (truncate_utf8)
    CHECK(has(r.notes, "comment_truncated"));
}
