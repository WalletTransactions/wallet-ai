#include "expense_check.hpp"

#include <algorithm>

#include "expense/text.hpp"

namespace expense_check {

Result check(const llm::Extraction& e, const std::string& text, const parser::Names& names, const parser::Now& now) {
    Result r;

    // Сумма: "1200,50" -> "1200.5" тем же разбором, что у строгого формата.
    parser::ParseError::Reason why = parser::ParseError::Reason::BadAmount;
    auto amount = parser::parse_amount(e.amount, &why);
    if (!amount) {
        r.notes.push_back(why == parser::ParseError::Reason::ZeroAmount ? "zero_amount" : "bad_amount");
        return r;
    }
    auto in_text = parser::amounts_in_text(text, names);
    if (std::find(in_text.begin(), in_text.end(), amount->amount) == in_text.end()) {
        r.notes.push_back("amount_not_in_text");
        return r;
    }

    // Дата: пусто — сегодня.
    std::string date = e.date.empty() ? now.date : e.date;
    switch (parser::check_date(date, now.date, kMaxAgeDays)) {
    case parser::DateCheck::Ok:
        break;
    case parser::DateCheck::Bad:
        r.notes.push_back("bad_date");
        return r;
    case parser::DateCheck::Future:
        r.notes.push_back("future_date");
        return r;
    case parser::DateCheck::TooOld:
        r.notes.push_back("too_old");
        return r;
    }

    parser::ParsedExpense& p = r.expense;
    p.amount = amount->amount;
    p.fraction_digits = amount->fraction_digits;
    p.datetime = parser::datetime_for(date, now);
    // Категория и кошелёк — как есть: схема уже ограничила их именами
    // пользователя или пустой строкой (тогда resolver спросит).
    p.category = e.category;
    p.wallet = e.wallet;
    // Валюта — только знакомая: подсказка лишь сужает выбор кошелька, но
    // мусорный код от модели выбор сломал бы.
    if (!e.currency.empty()) {
        std::string iso = parser::currency_of(e.currency, names);
        if (iso.empty()) {
            r.notes.push_back("currency_dropped");
        } else {
            p.currency_hint = iso;
        }
    }
    p.comment = text::truncate_utf8(e.comment, kMaxCommentBytes);
    if (p.comment != e.comment) r.notes.push_back("comment_truncated");
    r.ok = true;
    return r;
}

} // namespace expense_check
