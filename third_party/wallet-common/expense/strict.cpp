#include "expense/strict.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "expense/text.hpp"

namespace parser {

namespace {

// Встроенные синонимы валют — слова, которых нет ни в iso_code, ни в symbol
// справочника. Ключ — normalize_name без хвостовой точки ("руб." → "руб").
struct Synonym {
    const char* word;
    const char* iso_code;
};
constexpr Synonym kSynonyms[] = {
    {"₽", "RUB"},     {"р", "RUB"},       {"руб", "RUB"},     {"рубль", "RUB"},  {"рубля", "RUB"},
    {"рублей", "RUB"}, {"€", "EUR"},      {"евро", "EUR"},    {"$", "USD"},      {"долл", "USD"},
    {"доллар", "USD"}, {"доллара", "USD"}, {"долларов", "USD"}, {"бакс", "USD"},  {"бакса", "USD"},
    {"баксов", "USD"},
};

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!is_digit(c)) return false;
    }
    return true;
}

std::string upper_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// normalize_name без хвостовых точек: "Руб." == "руб", символ "р." == "р".
std::string normalize_currency(const std::string& s) {
    std::string n = text::normalize_name(s);
    while (!n.empty() && n.back() == '.') n.pop_back();
    return n;
}

} // namespace

std::string currency_of(const std::string& token, const Names& names) {
    std::string n = normalize_currency(token);
    if (n.empty()) return {};
    for (const auto& c : names.currencies) {
        if (!c.iso_code.empty() && normalize_currency(c.iso_code) == n) return c.iso_code;
        if (!c.symbol.empty() && normalize_currency(c.symbol) == n) return c.iso_code;
    }
    for (const auto& s : kSynonyms) {
        if (n == s.word) return s.iso_code;
    }
    // ISO-код, которого нет у кошельков пользователя, — всё равно валюта:
    // resolver ответит «нет кошелька в USD», а не примет "usd" за категорию.
    if (n.size() == 3 && std::isalpha(static_cast<unsigned char>(n[0])) &&
        std::isalpha(static_cast<unsigned char>(n[1])) && std::isalpha(static_cast<unsigned char>(n[2]))) {
        static const char* const kCommonIso[] = {"rub", "usd", "eur", "gbp", "cny", "kzt", "try",
                                                 "gel", "amd", "byn", "uah", "chf", "jpy", "aed"};
        for (const char* iso : kCommonIso) {
            if (n == iso) return upper_ascii(n);
        }
    }
    return {};
}

namespace {

// Токены по пробелам, включая неразрывный (U+00A0) и узкий неразрывный
// (U+202F) — так «1 200» из скопированной суммы тоже делится на группы.
std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isspace(c) != 0) {
            flush();
            ++i;
        } else if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
            flush();
            i += 2;
        } else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(s[i + 2]) == 0xAF) {
            flush();
            i += 3;
        } else {
            cur.push_back(s[i]);
            ++i;
        }
    }
    flush();
    return out;
}

// Комментарий после "//" — как написан (переносы строк внутри сохраняются),
// только без пробелов по краям.
std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

std::string join(const std::vector<std::string>& tokens, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to; ++i) {
        if (!out.empty()) out += ' ';
        out += tokens[i];
    }
    return out;
}

// Сколько первых токенов span образуют имя из списка, иначе 1 (неизвестное
// слово — resolver предложит кандидатов). 0 — span пуст. Точное ли это
// совпадение и не неоднозначен ли префикс, решает resolver.
//
// Побеждает самый длинный отрезок, который совпадает с именем или является
// его префиксом, с одной оговоркой. Префикс, оборванный посреди слова
// ("кафе и рест" → «Кафе и рестораны»), — явное сокращение, он выигрывает
// сразу. Префикс из нескольких слов, кончающийся на целом слове ("кафе и" в
// "кафе и пицца"), проигрывает более короткому точному совпадению («Кафе» с
// комментарием «и пицца»); если точного нет, берётся он.
std::size_t match_name_span(const std::vector<std::string>& span, const std::vector<std::string>& names) {
    if (span.empty()) return 0;
    std::vector<std::string> normalized;
    normalized.reserve(names.size());
    for (const auto& n : names) normalized.push_back(text::normalize_name(n));

    std::size_t word_boundary_prefix = 0;
    for (std::size_t k = span.size(); k >= 1; --k) {
        std::string q = text::normalize_name(join(span, 0, k));
        bool boundary = false;
        for (const auto& n : normalized) {
            if (n == q) return k;
            if (word_boundary_prefix != 0 || n.compare(0, q.size(), q) != 0) continue;
            if (k == 1 || n[q.size()] != ' ') return k; // оборвано посреди слова
            boundary = true;
        }
        if (boundary) word_boundary_prefix = k;
    }
    return word_boundary_prefix != 0 ? word_boundary_prefix : 1;
}

// --- Даты ------------------------------------------------------------------

struct Ymd {
    int y = 0;
    int m = 0;
    int d = 0;
};

bool operator<(const Ymd& a, const Ymd& b) {
    if (a.y != b.y) return a.y < b.y;
    if (a.m != b.m) return a.m < b.m;
    return a.d < b.d;
}

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && is_leap(y) ? 29 : kDays[m - 1];
}

bool valid(const Ymd& v) { return v.m >= 1 && v.m <= 12 && v.d >= 1 && v.d <= days_in_month(v.y, v.m); }

// days_from_civil / civil_from_days (H. Hinnant) — сдвиг на N дней без
// часовых поясов: «вчера» считается от уже локальной даты пользователя.
long days_from_civil(Ymd v) {
    int y = v.y - (v.m <= 2 ? 1 : 0);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (v.m + (v.m > 2 ? -3 : 9)) + 2) / 5 + v.d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

Ymd civil_from_days(long z) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    Ymd v;
    v.d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    v.m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    v.y = static_cast<int>(yoe + era * 400 + (v.m <= 2 ? 1 : 0));
    return v;
}

bool parse_iso_date(const std::string& s, Ymd& out) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    if (!all_digits(s.substr(0, 4)) || !all_digits(s.substr(5, 2)) || !all_digits(s.substr(8, 2))) return false;
    out = {std::stoi(s.substr(0, 4)), std::stoi(s.substr(5, 2)), std::stoi(s.substr(8, 2))};
    return valid(out);
}

std::string format_ymd(const Ymd& v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", v.y, v.m, v.d);
    return buf;
}

enum class DateKind { NotDate, Ok, Bad, Future };

// "сегодня" / "вчера" / "позавчера" / "DD.MM" / "DD.MM.YYYY". NotDate —
// токен вообще не похож на дату (тогда это слово категории или комментария).
DateKind parse_date_token(const std::string& token, const Ymd& today, Ymd& out) {
    std::string n = text::normalize_name(token);
    if (n == "сегодня" || n == "вчера" || n == "позавчера") {
        int offset = n == "сегодня" ? 0 : (n == "вчера" ? -1 : -2);
        out = civil_from_days(days_from_civil(today) + offset);
        return DateKind::Ok;
    }

    std::size_t dot1 = token.find('.');
    if (dot1 == std::string::npos) return DateKind::NotDate;
    std::size_t dot2 = token.find('.', dot1 + 1);
    std::string dd = token.substr(0, dot1);
    std::string mm = token.substr(dot1 + 1, dot2 == std::string::npos ? std::string::npos : dot2 - dot1 - 1);
    std::string yyyy = dot2 == std::string::npos ? std::string{} : token.substr(dot2 + 1);
    if (!all_digits(dd) || dd.size() > 2 || !all_digits(mm) || mm.size() > 2) return DateKind::NotDate;
    if (dot2 != std::string::npos && (!all_digits(yyyy) || yyyy.size() != 4)) return DateKind::NotDate;

    Ymd v{0, std::stoi(mm), std::stoi(dd)};
    if (yyyy.empty()) {
        // DD.MM без года — ближайший такой день, не позже сегодня: «31.12»,
        // набранное 2 января, — это прошлый год.
        v.y = today.y;
        if (today < Ymd{today.y, v.m, v.d}) --v.y;
        if (!valid(v)) return DateKind::Bad;
    } else {
        v.y = std::stoi(yyyy);
        if (!valid(v)) return DateKind::Bad;
        if (today < v) return DateKind::Future;
    }
    out = v;
    return DateKind::Ok;
}

// --- Сумма ------------------------------------------------------------------

// Токен вида [валюта]<цифры>[валюта]: "$12", "350₽", "1200,50".
struct AmountToken {
    std::string prefix;
    std::string core;
    std::string suffix;
};

AmountToken split_amount_token(const std::string& token) {
    AmountToken t;
    std::size_t i = 0;
    while (i < token.size() && !is_digit(token[i]) && token[i] != '.' && token[i] != ',') ++i;
    t.prefix = token.substr(0, i);
    std::size_t j = i;
    while (j < token.size() && (is_digit(token[j]) || token[j] == '.' || token[j] == ',')) ++j;
    t.core = token.substr(i, j - i);
    t.suffix = token.substr(j);
    return t;
}

bool is_thousands_group(const std::string& core) {
    if (core.size() < 3 || !all_digits(core.substr(0, 3))) return false;
    if (core.size() == 3) return true;
    return (core[3] == '.' || core[3] == ',') && all_digits(core.substr(4));
}

Result fail(ParseError::Reason reason) {
    Result r;
    r.error.reason = reason;
    return r;
}

} // namespace

const char* reason_name(ParseError::Reason reason) {
    switch (reason) {
    case ParseError::Reason::NoAmount:
        return "no_amount";
    case ParseError::Reason::BadAmount:
        return "bad_amount";
    case ParseError::Reason::ZeroAmount:
        return "zero_amount";
    case ParseError::Reason::BadDate:
        return "bad_date";
    case ParseError::Reason::FutureDate:
        return "future_date";
    }
    return "unknown";
}

std::optional<Amount> parse_amount(const std::string& core, ParseError::Reason* why) {
    auto fail_amount = [why](ParseError::Reason r) -> std::optional<Amount> {
        if (why != nullptr) *why = r;
        return std::nullopt;
    };
    std::size_t sep = core.find_first_of(".,");
    if (sep != std::string::npos && core.find_first_of(".,", sep + 1) != std::string::npos) {
        return fail_amount(ParseError::Reason::BadAmount); // "1,200.50" — какой из них десятичный?
    }
    std::string int_part = sep == std::string::npos ? core : core.substr(0, sep);
    std::string frac_part = sep == std::string::npos ? std::string{} : core.substr(sep + 1);
    if (!all_digits(int_part) || (sep != std::string::npos && !all_digits(frac_part))) {
        return fail_amount(ParseError::Reason::BadAmount);
    }
    while (int_part.size() > 1 && int_part[0] == '0') int_part.erase(0, 1);
    while (!frac_part.empty() && frac_part.back() == '0') frac_part.pop_back();
    // numeric(20,8) в operation.amount: 12 знаков до точки, 8 после.
    if (int_part.size() > 12 || frac_part.size() > 8) return fail_amount(ParseError::Reason::BadAmount);
    if (int_part == "0" && frac_part.empty()) return fail_amount(ParseError::Reason::ZeroAmount);
    Amount a;
    a.amount = frac_part.empty() ? int_part : int_part + "." + frac_part;
    a.fraction_digits = static_cast<int>(frac_part.size());
    return a;
}

std::vector<std::string> amounts_in_text(const std::string& text, const Names& names) {
    std::vector<std::string> out;
    auto add = [&out](std::string core) {
        while (!core.empty() && (core.back() == '.' || core.back() == ',')) core.pop_back(); // «450,» в конце фразы
        if (auto a = parse_amount(core)) {
            if (std::find(out.begin(), out.end(), a->amount) == out.end()) out.push_back(a->amount);
        }
    };
    std::vector<std::string> tokens = tokenize(text);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        AmountToken t = split_amount_token(tokens[i]);
        if (t.core.find_first_of("0123456789") == std::string::npos) continue;
        add(t.core);
        // Группы тысяч, как в parse_strict: «1 200 000,50». Каждое число
        // по отдельности уже добавлено — «кофе 200 450» даёт и 200, и 450, и
        // 200450: для проверки «есть в тексте» лишние кандидаты не мешают.
        if (!t.suffix.empty() || !all_digits(t.core) || t.core.size() > 3) continue;
        std::string core = t.core;
        for (std::size_t j = i + 1; j < tokens.size(); ++j) {
            AmountToken g = split_amount_token(tokens[j]);
            if (!g.prefix.empty() || !is_thousands_group(g.core)) break;
            if (!g.suffix.empty() && currency_of(g.suffix, names).empty()) break;
            core += g.core;
            add(core);
            if (!all_digits(g.core) || !g.suffix.empty()) break;
        }
    }
    return out;
}

DateCheck check_date(const std::string& ymd, const std::string& today, int max_age_days) {
    Ymd v;
    Ymd t;
    if (!parse_iso_date(ymd, v) || !parse_iso_date(today, t)) return DateCheck::Bad;
    if (t < v) return DateCheck::Future;
    if (days_from_civil(t) - days_from_civil(v) > max_age_days) return DateCheck::TooOld;
    return DateCheck::Ok;
}

std::string datetime_for(const std::string& ymd, const Now& now) {
    Ymd v;
    Ymd t;
    if (!parse_iso_date(ymd, v) || !parse_iso_date(now.date, t)) return {};
    return format_ymd(v) + " " + (v < t ? std::string("12:00:00") : now.time);
}

Result parse_strict(const std::string& text, const Names& names, const Now& now) {
    Ymd today;
    if (!parse_iso_date(now.date, today)) return fail(ParseError::Reason::BadDate);

    // Комментарий после "//" — как есть, без разбора.
    std::string body = text;
    std::string slash_comment;
    std::size_t slashes = text.find("//");
    if (slashes != std::string::npos) {
        body = text.substr(0, slashes);
        slash_comment = text.substr(slashes + 2);
    }

    std::vector<std::string> tokens = tokenize(body);
    if (tokens.empty()) return fail(ParseError::Reason::NoAmount);

    ParsedExpense e;

    // Сумма: первый токен плюс группы тысяч ("1 200 000,50").
    AmountToken first = split_amount_token(tokens[0]);
    if (first.core.find_first_of("0123456789") == std::string::npos) return fail(ParseError::Reason::NoAmount);
    if (!first.prefix.empty()) {
        if (first.prefix == "-" || first.prefix == "+") return fail(ParseError::Reason::BadAmount);
        e.currency_hint = currency_of(first.prefix, names);
        if (e.currency_hint.empty()) return fail(ParseError::Reason::NoAmount);
    }
    std::string core = first.core;
    std::string suffix = first.suffix;
    std::size_t i = 1;
    if (suffix.empty() && all_digits(core) && core.size() <= 3) {
        while (i < tokens.size()) {
            AmountToken group = split_amount_token(tokens[i]);
            if (!group.prefix.empty() || !is_thousands_group(group.core)) break;
            if (!group.suffix.empty() && currency_of(group.suffix, names).empty()) break;
            core += group.core;
            suffix = group.suffix;
            ++i;
            if (!all_digits(group.core) || !suffix.empty()) break; // дробная часть или валюта — сумма кончилась
        }
    }
    if (!suffix.empty()) {
        std::string cur = currency_of(suffix, names);
        if (cur.empty() || !e.currency_hint.empty()) return fail(ParseError::Reason::BadAmount);
        e.currency_hint = cur;
    }

    ParseError::Reason amount_error = ParseError::Reason::BadAmount;
    auto amount = parse_amount(core, &amount_error);
    if (!amount) return fail(amount_error);
    e.amount = amount->amount;
    e.fraction_digits = amount->fraction_digits;

    // Валюта отдельным словом сразу после суммы.
    if (e.currency_hint.empty() && i < tokens.size()) {
        e.currency_hint = currency_of(tokens[i], names);
        if (!e.currency_hint.empty()) ++i;
    }

    // Категория: слова до первого '@' или даты.
    Ymd date = today;
    bool has_date = false;
    auto is_stop = [&](const std::string& token) {
        Ymd ignored;
        return token[0] == '@' || parse_date_token(token, today, ignored) != DateKind::NotDate;
    };
    std::vector<std::string> span;
    while (i < tokens.size() && !is_stop(tokens[i])) span.push_back(tokens[i++]);
    std::size_t k = match_name_span(span, names.categories);
    e.category = join(span, 0, k);
    std::vector<std::string> comment_words(span.begin() + static_cast<long>(k), span.end());

    // Хвост: @кошелёк и дата в любом порядке, остальное — в комментарий.
    while (i < tokens.size()) {
        const std::string& token = tokens[i];
        if (token[0] == '@' && e.wallet.empty()) {
            std::vector<std::string> wspan;
            if (token.size() > 1) wspan.push_back(token.substr(1));
            ++i;
            while (i < tokens.size() && !is_stop(tokens[i])) wspan.push_back(tokens[i++]);
            std::size_t wk = match_name_span(wspan, names.wallets);
            e.wallet = join(wspan, 0, wk);
            // «@тинькофф евро» без кошелька «Тинькофф евро» — «евро» остаётся
            // подсказкой валюты.
            if (wk < wspan.size() && e.currency_hint.empty()) {
                e.currency_hint = currency_of(wspan[wk], names);
                if (!e.currency_hint.empty()) ++wk;
            }
            for (; wk < wspan.size(); ++wk) comment_words.push_back(wspan[wk]);
            continue;
        }
        Ymd parsed;
        DateKind kind = has_date ? DateKind::NotDate : parse_date_token(token, today, parsed);
        if (kind == DateKind::Bad) return fail(ParseError::Reason::BadDate);
        if (kind == DateKind::Future) return fail(ParseError::Reason::FutureDate);
        if (kind == DateKind::Ok) {
            date = parsed;
            has_date = true;
        } else {
            comment_words.push_back(token);
        }
        ++i;
    }

    e.datetime = format_ymd(date) + " " + (date < today ? std::string("12:00:00") : now.time);

    std::string comment = join(comment_words, 0, comment_words.size());
    std::string tail = trim(slash_comment);
    if (!tail.empty()) comment = comment.empty() ? tail : comment + " " + tail;
    e.comment = comment;

    Result r;
    r.ok = true;
    r.expense = e;
    return r;
}

} // namespace parser
