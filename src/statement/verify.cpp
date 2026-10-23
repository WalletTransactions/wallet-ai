#include "statement/verify.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>

#include "statement/textutil.hpp"

namespace stmt {

namespace {

const char* const kMonths[] = {"января", "февраля", "марта",    "апреля",  "мая",    "июня",
                               "июля",   "августа", "сентября", "октября", "ноября", "декабря"};

const char* const kWeekdays[] = {"понедельник", "вторник", "среда", "среду", "четверг", "пятница", "пятницу",
                                 "суббота", "субботу", "воскресенье", "пн", "вт", "ср", "чт", "пт", "сб", "вс",
                                 "сегодня", "вчера", "г", "года"};

const std::regex& full_re() {
    static const std::regex re(R"((^|[^\d])(\d{1,2})[./](\d{1,2})[./](\d{4}|\d{2})(?!\d))");
    return re;
}
// «01-08-2026» — только с полным годом: «01-08-26» похоже на номер.
// «01/08/2026» ловит full_re вместе с точками.
const std::regex& dash_re() {
    static const std::regex re(R"((^|[^\d-])(\d{1,2})-(\d{1,2})-(\d{4})(?![\d-]))");
    return re;
}
// Строка начинается с полной даты (CSV «01-08-2026 15:58:01;…», выписка «12.09.2026 …»).
const std::regex& leading_date_re() {
    static const std::regex re(R"(^ *(\d{1,2}[./-]\d{1,2}[./-](\d{4}|\d{2})|\d{4}-\d{1,2}-\d{1,2})(?![\d.,]))");
    return re;
}
const std::regex& iso_re() {
    static const std::regex re(R"((^|[^\d])(\d{4})-(\d{1,2})-(\d{1,2})(?!\d))");
    return re;
}
const std::regex& ddmm_re() {
    static const std::regex re(R"((^|[^\d.,])(\d{1,2})\.(\d{2})(?![.,]?\d))");
    return re;
}
const std::regex& month_re() {
    static const std::regex re(
        R"((^|[^\d])(\d{1,2}) (января|февраля|марта|апреля|мая|июня|июля|августа|сентября|октября|ноября|декабря)( (\d{4}))?)");
    return re;
}
const std::regex& time_re() {
    static const std::regex re(R"(\d{1,2}:\d{2}(:\d{2})?)");
    return re;
}
const std::regex& leading_ddmm_re() {
    static const std::regex re(R"(^ *\d{1,2}\.\d{2}(?![.,]?\d))");
    return re;
}
// Знак — минус, плюс или «−» (U+2212); разряды — пробел или запятая по три цифры.
const std::regex& amount_re() {
    static const std::regex re("(^|[^0-9A-Za-z.,])(-|\\+|\xE2\x88\x92)? ?(\\d{1,3}(?:[ ,]\\d{3})+|\\d+)(?:[.,](\\d{1,2}))?(?!\\d)");
    return re;
}

bool allowed(const std::vector<std::string>& formats, const char* f) {
    return formats.empty() || std::find(formats.begin(), formats.end(), f) != formats.end();
}

bool valid_md(int m, int d) { return m >= 1 && m <= 12 && d >= 1 && d <= 31; }

int year_of(const std::string& digits) {
    int y = std::stoi(digits);
    return digits.size() == 2 ? 2000 + y : y;
}

std::string prepare(const std::string& line) { return normalize_spaces(lower(line)); }

// Вырезает даты и время, чтобы их цифры не стали суммами.
std::string strip_dates(const std::string& s) {
    std::string out = std::regex_replace(s, full_re(), "$1 ");
    out = std::regex_replace(out, dash_re(), "$1 ");
    out = std::regex_replace(out, iso_re(), "$1 ");
    out = std::regex_replace(out, month_re(), "$1 ");
    out = std::regex_replace(out, time_re(), " ");
    out = std::regex_replace(out, leading_ddmm_re(), " ");
    return out;
}

long long cents_of(const std::string& int_part, const std::string& frac) {
    long long v = 0;
    for (char c : int_part) {
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
    }
    v *= 100;
    if (frac.size() == 1) v += (frac[0] - '0') * 10;
    if (frac.size() == 2) v += (frac[0] - '0') * 10 + (frac[1] - '0');
    return v;
}

bool contains_word(const std::string& hay_lower, const std::string& word_lower) {
    return !word_lower.empty() && hay_lower.find(word_lower) != std::string::npos;
}

const char* const kCurrencyTokens[] = {"USD", "EUR", "GBP", "CNY", "KZT", "TRY", "GEL", "AMD",
                                       "BYN", "UAH", "CHF", "JPY", "AED", "RUB", "€", "$", "£"};

std::string currency_of_token(const std::string& t) {
    if (t == "€") return "EUR";
    if (t == "$") return "USD";
    if (t == "£") return "GBP";
    return t;
}

// Чужая валюта, явно названная в строке («EUR 12.50», «12.50 $»).
std::string foreign_currency_in(const std::string& line, const std::string& wallet) {
    std::string s = " " + upper(normalize_spaces(line)) + " ";
    for (const char* tok : kCurrencyTokens) {
        std::string t = tok;
        std::size_t pos = 0;
        while ((pos = s.find(t, pos)) != std::string::npos) {
            bool symbol = t == "€" || t == "$" || t == "£";
            auto before = static_cast<unsigned char>(s[pos - 1]);
            auto after = static_cast<unsigned char>(s[pos + t.size()]);
            bool bounded = symbol || ((before == ' ' || before == '(' || std::isdigit(before) != 0) &&
                                      (after == ' ' || after == ')' || after == '.' || after == ',' ||
                                       std::isdigit(after) != 0));
            std::string iso = currency_of_token(t);
            if (bounded && iso != wallet) return iso;
            pos += t.size();
        }
    }
    return {};
}

} // namespace

long days_from_civil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool parse_iso(const std::string& s, Date& out) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (std::size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    Date d{std::stoi(s.substr(0, 4)), std::stoi(s.substr(5, 2)), std::stoi(s.substr(8, 2))};
    if (!valid_md(d.m, d.d)) return false;
    // 31 апреля, 30 февраля, 29 февраля не в високосный.
    if ((d.m == 2 && d.d > 29) || (d.d == 31 && (d.m == 4 || d.m == 6 || d.m == 9 || d.m == 11)) ||
        (d.m == 2 && d.d == 29 && !((d.y % 4 == 0 && d.y % 100 != 0) || d.y % 400 == 0))) {
        return false;
    }
    out = d;
    return true;
}

std::string to_iso(int y, int m, int d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return buf;
}

std::vector<Date> dates_in_line(const std::string& line, const std::vector<std::string>& formats) {
    std::vector<Date> out;
    std::string s = prepare(line);
    for (auto it = std::sregex_iterator(s.begin(), s.end(), full_re()); it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        bool four = m[4].length() == 4;
        if (!allowed(formats, four ? "DD.MM.YYYY" : "DD.MM.YY")) continue;
        Date d{year_of(m[4]), std::stoi(m[3]), std::stoi(m[2])};
        if (valid_md(d.m, d.d)) out.push_back(d);
    }
    if (allowed(formats, "DD-MM-YYYY")) {
        for (auto it = std::sregex_iterator(s.begin(), s.end(), dash_re()); it != std::sregex_iterator(); ++it) {
            Date d{std::stoi((*it)[4]), std::stoi((*it)[3]), std::stoi((*it)[2])};
            if (valid_md(d.m, d.d)) out.push_back(d);
        }
    }
    if (allowed(formats, "YYYY-MM-DD")) {
        for (auto it = std::sregex_iterator(s.begin(), s.end(), iso_re()); it != std::sregex_iterator(); ++it) {
            Date d{std::stoi((*it)[2]), std::stoi((*it)[3]), std::stoi((*it)[4])};
            if (valid_md(d.m, d.d)) out.push_back(d);
        }
    }
    if (allowed(formats, "DD.MM")) {
        for (auto it = std::sregex_iterator(s.begin(), s.end(), ddmm_re()); it != std::sregex_iterator(); ++it) {
            Date d{0, std::stoi((*it)[3]), std::stoi((*it)[2])};
            if (valid_md(d.m, d.d)) out.push_back(d);
        }
    }
    if (allowed(formats, "D MONTH")) {
        for (auto it = std::sregex_iterator(s.begin(), s.end(), month_re()); it != std::sregex_iterator(); ++it) {
            int month = 0;
            for (int i = 0; i < 12; ++i) {
                if ((*it)[3] == kMonths[i]) month = i + 1;
            }
            Date d{(*it)[5].matched ? std::stoi((*it)[5]) : 0, month, std::stoi((*it)[2])};
            if (valid_md(d.m, d.d)) out.push_back(d);
        }
    }
    return out;
}

std::vector<Amount> amounts_in_line(const std::string& line) {
    std::vector<Amount> out;
    std::string s = strip_dates(normalize_spaces(line));
    for (auto it = std::sregex_iterator(s.begin(), s.end(), amount_re()); it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        std::string sign = m[2];
        std::string int_part = m[3];
        std::string frac = m[4];
        Amount a;
        a.sign = sign.empty() ? 0 : (sign == "+" ? '+' : '-');
        a.two_decimals = frac.size() == 2;
        a.cents = cents_of(int_part, frac);
        out.push_back(a);
        // «2 450,00» после «кол-во»: разряды могли склеить два числа — хвосты
        // по границам групп тоже кандидаты (знак — только у полного числа).
        for (std::size_t p = 0; p < int_part.size(); ++p) {
            if (int_part[p] == ' ' || int_part[p] == ',') {
                Amount tail;
                tail.two_decimals = a.two_decimals;
                tail.cents = cents_of(int_part.substr(p + 1), frac);
                out.push_back(tail);
            }
        }
    }
    return out;
}

std::optional<long long> parse_amount(const std::string& raw) {
    std::string s;
    for (char c : normalize_spaces(raw)) {
        if (c != ' ') s.push_back(c);
    }
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) s.erase(0, 1);
    if (s.find(',') != std::string::npos && s.find('.') != std::string::npos) {
        s.erase(std::remove(s.begin(), s.end(), ','), s.end());
    } else {
        std::replace(s.begin(), s.end(), ',', '.');
    }
    auto dot = s.find('.');
    std::string int_part = s.substr(0, dot);
    std::string frac = dot == std::string::npos ? "" : s.substr(dot + 1);
    // «450.000» — модель дописала нули: три знака допустимы, если лишние нули.
    while (frac.size() > 2 && frac.back() == '0') frac.pop_back();
    if (int_part.empty() || int_part.size() > 15 || frac.size() > 2) return std::nullopt;
    for (char c : int_part + frac) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    return cents_of(int_part, frac);
}

std::string format_cents(long long cents) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%02lld", cents / 100, cents % 100);
    return buf;
}

bool money_line(const std::string& line) {
    auto amounts = amounts_in_line(line);
    for (const auto& a : amounts) {
        if (a.two_decimals) return true;
    }
    // Суммы без хвостовых нулей («-31», «-76.1»): строка с датой в начале и числом.
    return !amounts.empty() && std::regex_search(normalize_spaces(line), leading_date_re());
}

bool is_date_header(const std::string& line) {
    std::string s = prepare(line);
    if (dates_in_line(line).empty() || money_line(line)) return false;
    std::string rest = std::regex_replace(s, full_re(), "$1 ");
    rest = std::regex_replace(rest, iso_re(), "$1 ");
    rest = std::regex_replace(rest, month_re(), "$1 ");
    rest = std::regex_replace(rest, ddmm_re(), "$1 ");
    // Остаток — только дни недели, «г.», пунктуация.
    std::string word;
    auto flush = [&]() -> bool {
        if (word.empty()) return true;
        bool ok = false;
        for (const char* w : kWeekdays) ok = ok || word == w;
        word.clear();
        return ok;
    };
    for (char c : rest + " ") {
        auto u = static_cast<unsigned char>(c);
        if (std::isdigit(u) != 0) return false;
        if (std::isalpha(u) != 0 || u >= 0x80) {
            word.push_back(c);
        } else if (!flush()) {
            return false;
        }
    }
    return true;
}

bool is_total_line(const std::string& line, const std::vector<std::string>& extra_stop_words) {
    std::string s = lower(line);
    for (const char* w : {"итого", "остаток", "баланс"}) {
        if (contains_word(s, w)) return true;
    }
    for (const auto& w : extra_stop_words) {
        if (contains_word(s, lower(trim(w)))) return true;
    }
    return false;
}

Period find_period(const std::vector<std::string>& lines) {
    std::size_t n = std::min<std::size_t>(lines.size(), 40);
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<std::string> full;
        for (const auto& d : dates_in_line(lines[i])) {
            if (d.y != 0) full.push_back(to_iso(d.y, d.m, d.d));
        }
        if (full.size() >= 2) {
            auto [lo, hi] = std::minmax(full[0], full[1]);
            return {lo, hi};
        }
    }
    return {};
}

std::optional<Verified> verify_row(const Candidate& c, const VerifyContext& ctx) {
    const auto& text = *ctx.lines;
    Verified v;
    for (int n : c.lines) {
        if (n >= ctx.chunk_first && n <= ctx.chunk_last && n >= 1 && n <= static_cast<int>(text.size())) {
            v.lines.push_back(n);
        }
    }
    std::sort(v.lines.begin(), v.lines.end());
    v.lines.erase(std::unique(v.lines.begin(), v.lines.end()), v.lines.end());
    if (v.lines.empty()) return std::nullopt;

    v.description = trim(c.description);
    v.dir = c.dir == "income" || c.dir == "transfer" ? c.dir : "expense";
    v.currency = upper(trim(c.currency));

    // Сумма: модуль модели должен совпасть с числом из строк-источников.
    auto cents = parse_amount(c.amount);
    v.amount = cents ? format_cents(*cents) : "";
    const Amount* found = nullptr;
    std::vector<Amount> seen;
    std::string joined_lower;
    for (int n : v.lines) {
        for (const auto& a : amounts_in_line(text[n - 1])) seen.push_back(a);
        joined_lower += lower(text[n - 1]) + "\n";
    }
    for (const auto& a : seen) {
        if (cents && a.cents == *cents) {
            found = &a;
            break;
        }
    }
    if (found == nullptr) v.check = "amount_not_in_text";

    // Знак исходника перекрывает направление модели (переводы не трогаем).
    if (found != nullptr && found->sign == '-' && v.dir == "income") v.dir = "expense";
    if (found != nullptr && found->sign == '+' && v.dir == "expense") v.dir = "income";
    if (!ctx.rules.expense_word.empty() && v.dir == "income" &&
        contains_word(joined_lower, lower(ctx.rules.expense_word))) {
        v.dir = "expense";
    }
    // Переводы между своими счетами по словам профиля — сильнее знака.
    for (const auto& w : ctx.rules.transfer_words) {
        if (contains_word(joined_lower, lower(trim(w)))) v.dir = "transfer";
    }

    // Дата: в строках-источниках, в ближайшем заголовке выше или в контексте.
    Date md;
    Date today;
    bool date_ok = false;
    if (parse_iso(c.date, md) && parse_iso(ctx.today, today)) {
        v.date = c.date;
        long age = days_from_civil(today.y, today.m, today.d) - days_from_civil(md.y, md.m, md.d);
        bool in_window = age >= 0 && age <= 3 * 366;
        auto year_ok = [&](const Date& hit) {
            if (hit.y != 0) return hit.y == md.y;
            Date from;
            Date to;
            if (parse_iso(ctx.period.from, from) && parse_iso(ctx.period.to, to)) {
                return md.y == from.y || md.y == to.y;
            }
            // Периода нет — год создания импорта или прошлый (декабрь в январской выписке).
            return md.y == today.y || md.y == today.y - 1;
        };
        auto matches = [&](const std::vector<Date>& hits) {
            for (const auto& h : hits) {
                if (h.m == md.m && h.d == md.d && year_ok(h)) return true;
            }
            return false;
        };
        std::vector<Date> hits;
        for (int n : v.lines) {
            auto d = dates_in_line(text[n - 1], ctx.rules.date_formats);
            hits.insert(hits.end(), d.begin(), d.end());
        }
        bool header_found = false;
        for (int n = v.lines.front() - 1; n >= ctx.chunk_first && !header_found; --n) {
            if (is_date_header(text[n - 1])) {
                auto d = dates_in_line(text[n - 1]);
                hits.insert(hits.end(), d.begin(), d.end());
                header_found = true;
            }
        }
        if (!header_found && !ctx.date_context.empty()) {
            auto d = dates_in_line(ctx.date_context);
            hits.insert(hits.end(), d.begin(), d.end());
        }
        date_ok = in_window && matches(hits);
    }
    if (!date_ok && v.check.empty()) v.check = "date_not_in_text";

    // Валюта: названная моделью или явно стоящая в строке, если не кошелька.
    std::string foreign;
    if (!v.currency.empty() && v.currency != ctx.wallet_currency) foreign = v.currency;
    if (foreign.empty()) {
        for (int n : v.lines) {
            foreign = foreign_currency_in(text[n - 1], ctx.wallet_currency);
            if (!foreign.empty()) break;
        }
    }
    if (!foreign.empty()) {
        v.currency = foreign;
        if (v.check.empty()) v.check = "currency_mismatch";
    }
    return v;
}

std::vector<int> uncovered_lines(const std::vector<std::string>& lines, int first, int last,
                                 const std::set<int>& covered, const std::vector<std::string>& stop_words) {
    std::vector<int> out;
    for (int n = std::max(first, 1); n <= last && n <= static_cast<int>(lines.size()); ++n) {
        if (covered.count(n) != 0) continue;
        const auto& line = lines[n - 1];
        if (money_line(line) && !is_total_line(line, stop_words)) out.push_back(n);
    }
    return out;
}

} // namespace stmt
