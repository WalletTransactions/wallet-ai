// Чистые функции импорта выписок: проверка, нарезка, pattern, отпечатки,
// профили, извлечение текста (openspec/changes/add-statement-import).
#include <algorithm>
#include <cstdio>

#include "llm_client.hpp"
#include "statement/chunk.hpp"
#include "statement/pattern.hpp"
#include "statement/profiles.hpp"
#include "statement/source.hpp"
#include "statement/textutil.hpp"
#include "statement/verify.hpp"
#include "test_framework.hpp"

using namespace stmt;

namespace {

std::vector<long long> cents(const std::string& line) {
    std::vector<long long> out;
    for (const auto& a : amounts_in_line(line)) out.push_back(a.cents);
    return out;
}

bool has(const std::vector<long long>& v, long long x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// Минимальный PDF с одной строкой текста (или без текста) — pdftotext
// восстанавливает xref сам, смещения не нужны точными.
std::string tiny_pdf(const std::string& text) {
    std::string stream = text.empty() ? "" : "BT /F1 12 Tf 72 720 Td (" + text + ") Tj ET";
    std::string pdf = "%PDF-1.4\n";
    pdf += "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n";
    pdf += "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n";
    pdf += "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
           "/Resources << /Font << /F1 5 0 R >> >> >> endobj\n";
    pdf += "4 0 obj << /Length " + std::to_string(stream.size()) + " >> stream\n" + stream + "\nendstream endobj\n";
    pdf += "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n";
    pdf += "trailer << /Root 1 0 R >>\n%%EOF\n";
    return pdf;
}

std::string b64(const std::string& s) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (unsigned char c : s) {
        buf = (buf << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(a[(buf >> bits) & 0x3F]);
        }
    }
    if (bits > 0) out.push_back(a[(buf << (6 - bits)) & 0x3F]);
    while (out.size() % 4 != 0) out.push_back('=');
    return out;
}

VerifyContext ctx_for(const std::vector<std::string>& lines, const std::string& today = "2026-10-22") {
    VerifyContext c;
    c.lines = &lines;
    c.chunk_first = 1;
    c.chunk_last = static_cast<int>(lines.size());
    c.today = today;
    c.wallet_currency = "RUB";
    return c;
}

} // namespace

TEST(text_case_and_utf8) {
    CHECK_EQ(upper("Yandex такси ёж"), std::string("YANDEX ТАКСИ ЁЖ"));
    CHECK_EQ(lower("ИТОГО Ё"), std::string("итого ё"));
    CHECK(valid_utf8("привет"));
    CHECK(!valid_utf8("\xCF\xF0\xE8"));
    CHECK_EQ(cp1251_to_utf8("\xCF\xF0\xE8\xE2\xE5\xF2 \xA8"), std::string("Привет Ё"));
    CHECK_EQ(decode_text("\xEF\xBB\xBFдата"), std::string("дата"));
    CHECK_EQ(decode_text("\xC4\xE0\xF2\xE0"), std::string("Дата"));
    CHECK_EQ(normalize_spaces("1\xC2\xA0" "290,00"), std::string("1 290,00"));
}

TEST(amounts_and_dates_in_line) {
    // Дата в начале строки — не сумма; остаток — тоже сумма, его отсекает профиль/модель.
    auto a = amounts_in_line("12.09 KOFE -200,00 15 300,00");
    REQUIRE(a.size() >= 2u);
    CHECK_EQ(a[0].cents, 20000LL);
    CHECK(a[0].sign == '-');
    CHECK(has(cents("12.09 KOFE -200,00 15 300,00"), 1530000));
    CHECK(!has(cents("12.09 KOFE -200,00"), 1209));
    CHECK(has(cents("15.09 APTEKA 36.6 -1\xC2\xA0" "290,00"), 129000));
    CHECK(has(cents("Payment 1,290.00 USD"), 129000));
    CHECK(has(cents("12.09.2026 14:05 YANDEX*GO −450,00 ₽"), 45000));
    CHECK(!has(cents("12.09.2026 14:05 YANDEX*GO −450,00 ₽"), 1209));

    CHECK_EQ(*parse_amount("1 290,00"), 129000LL);
    CHECK_EQ(*parse_amount("1,290.00"), 129000LL);
    CHECK_EQ(*parse_amount("-450"), 45000LL);
    CHECK_EQ(*parse_amount("12.5"), 1250LL);
    CHECK(!parse_amount("abc"));
    CHECK_EQ(format_cents(129000), std::string("1290.00"));

    auto d = dates_in_line("12 сентября 2026, суббота");
    REQUIRE(d.size() == 1u);
    CHECK(d[0].y == 2026 && d[0].m == 9 && d[0].d == 12);
    CHECK(dates_in_line("12.09.26 KOFE")[0].y == 2026);
    CHECK(dates_in_line("2026-09-12 KOFE")[0].d == 12);
    CHECK(dates_in_line("12.09 KOFE")[0].y == 0);
    CHECK(dates_in_line("12.09 KOFE", {"DD.MM.YYYY"}).empty());

    CHECK(is_date_header("12 сентября 2026, суббота"));
    CHECK(is_date_header("12.09.2026"));
    CHECK(!is_date_header("12.09 KOFE"));
    CHECK(!is_date_header("12.09 KOFE 200,00"));

    CHECK(money_line("Итого расходов: 45 300,00"));
    CHECK(is_total_line("Итого расходов: 45 300,00", {}));
    CHECK(is_total_line("Исходящий ОСТАТОК 10,00", {}));
    CHECK(is_total_line("Кешбэк за месяц 120,00", {"кешбэк"}));
    CHECK(!money_line("APTEKA 36.6"));

    auto p = find_period({"Выписка по счёту", "за период с 01.09.2026 по 30.09.2026"});
    CHECK_EQ(p.from, std::string("2026-09-01"));
    CHECK_EQ(p.to, std::string("2026-09-30"));
}

TEST(verify_amount_and_date) {
    std::vector<std::string> lines = {"Выписка", "12.09 YANDEX*GO MOSCOW -450,00", "12.09 KOFE 1 290,00"};
    auto ctx = ctx_for(lines);

    auto ok = verify_row({{2}, "2026-09-12", "450.00", "expense", "", "YANDEX*GO MOSCOW"}, ctx);
    REQUIRE(ok.has_value());
    CHECK(ok->check.empty());
    CHECK_EQ(ok->amount, std::string("450.00"));

    // Модель ошиблась в сумме.
    auto wrong = verify_row({{3}, "2026-09-12", "1209.00", "expense", "", "KOFE"}, ctx);
    CHECK_EQ(wrong->check, std::string("amount_not_in_text"));

    // Выдуманная дата: в строке только «12.09».
    auto bad_date = verify_row({{3}, "2026-09-13", "1290.00", "expense", "", "KOFE"}, ctx);
    CHECK_EQ(bad_date->check, std::string("date_not_in_text"));

    // Будущее и старше 3 лет не подтверждаются.
    CHECK_EQ(verify_row({{2}, "2026-09-12", "450", "expense", "", "x"}, ctx_for(lines, "2026-09-01"))->check,
             std::string("date_not_in_text"));
    // Номер строки вне фрагмента — строку нечем подтвердить.
    CHECK(!verify_row({{99}, "2026-09-12", "450", "expense", "", "x"}, ctx).has_value());

    // Знак исходника перекрывает направление модели.
    CHECK_EQ(verify_row({{2}, "2026-09-12", "450", "income", "", "x"}, ctx)->dir, std::string("expense"));
}

TEST(verify_date_from_header_and_context) {
    std::vector<std::string> lines = {"12 сентября 2026", "KOFE 200,00", "APTEKA 300,00"};
    auto ctx = ctx_for(lines);
    CHECK(verify_row({{3}, "2026-09-12", "300", "expense", "", "APTEKA"}, ctx)->check.empty());

    // Фрагмент начинается без даты — контекст из прошлого фрагмента.
    std::vector<std::string> tail = {"KOFE 200,00"};
    auto c2 = ctx_for(tail);
    c2.date_context = "12.09.2026";
    CHECK(verify_row({{1}, "2026-09-12", "200", "expense", "", "KOFE"}, c2)->check.empty());
    c2.date_context.clear();
    CHECK_EQ(verify_row({{1}, "2026-09-12", "200", "expense", "", "KOFE"}, c2)->check, std::string("date_not_in_text"));
}

TEST(verify_currency_and_profile_rules) {
    std::vector<std::string> lines = {"12.09 CAFE PARIS EUR 12.50", "12.09 Списание MAGAZIN 300,00"};
    auto ctx = ctx_for(lines);
    CHECK_EQ(verify_row({{1}, "2026-09-12", "12.50", "expense", "", "CAFE PARIS"}, ctx)->check,
             std::string("currency_mismatch"));
    CHECK_EQ(verify_row({{1}, "2026-09-12", "12.50", "expense", "EUR", "CAFE PARIS"}, ctx)->currency,
             std::string("EUR"));
    ctx.rules.expense_word = "Списание";
    CHECK_EQ(verify_row({{2}, "2026-09-12", "300", "income", "", "MAGAZIN"}, ctx)->dir, std::string("expense"));
}

TEST(uncovered_lines_skip_totals) {
    std::vector<std::string> lines = {"15.09 APTEKA 36.6 -1 290,00", "Итого расходов: 45 300,00", "Шапка без сумм",
                                      "16.09 KOFE 200,00"};
    auto u = uncovered_lines(lines, 1, 4, {4}, {});
    CHECK_EQ(u.size(), 1u);
    CHECK_EQ(u[0], 1);
}

TEST(chunks_cut_before_dates_and_carry_context) {
    // Многострочные операции: дата на одной строке, описание с суммой — на следующей.
    std::vector<std::string> lines;
    for (int i = 1; i <= 20; ++i) {
        lines.push_back(std::to_string(10 + i % 18) + ".09.2026");
        lines.push_back("OPERATION NUMBER " + std::to_string(i) + " -1 000,00");
    }
    auto plans = plan_chunks(lines, 120);
    REQUIRE(plans.size() > 1u);
    for (const auto& p : plans) {
        CHECK(starts_with_date(lines[p.first_line - 1])); // операция целиком в одном фрагменте
    }
    CHECK_EQ(plans.back().last_line, static_cast<int>(lines.size()));
    CHECK(!plans[1].date_context.empty());

    // Шапка без сумм отдельным фрагментом не уходит; строка длиннее лимита — целиком.
    std::vector<std::string> mixed = {"Банк", "Клиент", std::string(300, 'x') + " 100,00"};
    auto p2 = plan_chunks(mixed, 50);
    REQUIRE(!p2.empty());
    CHECK_EQ(p2.back().last_line, 3);

    CHECK_EQ(numbered_text({"a", "b"}, 2, 2), std::string("2| b\n"));
    CHECK_EQ(llm::rows_max_tokens(30), 2000);
    CHECK_EQ(llm::rows_max_tokens(100), 2400);
}

TEST(pattern_and_fingerprint) {
    CHECK_EQ(pattern_of("Yandex*Go  MOSCOW RUS 4021"), std::string("YANDEX*GO"));
    CHECK_EQ(pattern_of("PYATEROCHKA 1234 SANKT-PETERBU RUS"), std::string("PYATEROCHKA"));
    CHECK_EQ(pattern_of("Перевод Иванову"), std::string("ПЕРЕВОД ИВАНОВУ"));
    CHECK_EQ(pattern_of("APTEKA 36.6"), std::string("APTEKA 36.6"));

    auto a = fingerprint("w", "2026-09-12", "200.00", "expense", "KOFE", 0);
    CHECK_EQ(a, fingerprint("w", "2026-09-12", "200.00", "expense", "KOFE", 0)); // та же выписка — тот же
    CHECK(a != fingerprint("w", "2026-09-12", "200.00", "expense", "KOFE", 1));  // две одинаковые покупки
    CHECK_EQ(a.size(), 64u);
    CHECK_EQ(op_uuid(a), op_uuid(a));
    CHECK(op_uuid(a) != op_uuid(fingerprint("w", "2026-09-12", "200.00", "expense", "KOFE", 1)));
}

TEST(profile_parse_detect_and_budget) {
    const char* good = "# version: test-1\n# комментарий\nid: tbank\nname: ТБанк\n[signature]\nАО «ТБанк»\n"
                       "Дата операции\n[hints]\nПоследняя колонка — остаток, не сумма.\n[examples]\n"
                       "12.09 KOFE -200,00 15 300,00\n=> {\"date\":\"2026-09-12\",\"amount\":\"200.00\","
                       "\"dir\":\"expense\",\"description\":\"KOFE\"}\n[check]\nstop_words: Кешбэк\n"
                       "date_formats: DD.MM, DD.MM.YYYY\nexpense_word: Списание\n";
    auto p = parse_profile(good);
    CHECK_EQ(p.id, std::string("tbank"));
    CHECK_EQ(p.examples.size(), 1u);
    CHECK_EQ(p.check.stop_words.size(), 1u);
    CHECK_EQ(p.check.expense_word, std::string("Списание"));

    bool threw = false;
    try {
        parse_profile("id: sber\nname: Сбер\n[signature]\nСбербанк\n");
    } catch (const std::exception&) {
        threw = true; // нет # version:
    }
    CHECK(threw);

    auto other = p;
    other.id = "other";
    std::vector<std::string> head = {"АО «ТБанк»", "Дата операции | Сумма | Описание"};
    CHECK_EQ(matching_profiles({p}, head).size(), 1u);
    CHECK_EQ(matching_profiles({p, other}, head).size(), 2u); // ambiguous решает вызывающий
    CHECK(matching_profiles({p}, {"Сбербанк"}).empty());

    // CSV за два месяца — один отпечаток; другой заголовок — другой.
    auto f1 = format_fingerprint({"Дата;Сумма;Описание", "01.09.2026;100,00;A"}, true);
    auto f2 = format_fingerprint({"", "Дата;Сумма;Описание", "01.10.2026;5,00;B"}, true);
    CHECK_EQ(f1, f2);
    CHECK(f1 != format_fingerprint({"Date;Amount", "x"}, true));
    CHECK(!format_fingerprint({"Банк", "Дата операции  Сумма  Валюта  Описание"}, false).empty());
    CHECK(format_fingerprint({"Банк", "Привет"}, false).empty());

    // Бюджет: свои примеры раньше встроенных, всё в пределах max_tokens.
    std::vector<Example> own = {{"16.09 OWN 100,00", {{"date", "2026-09-16"}, {"amount", "100.00"},
                                                       {"dir", "expense"}, {"description", "OWN"}}}};
    auto prompt = profile_prompt(&p, own, 500);
    CHECK(prompt.find("остаток") != std::string::npos);
    CHECK(prompt.find("OWN") < prompt.find("KOFE"));
    CHECK(estimate_tokens(prompt) <= 500u);
    CHECK(estimate_tokens(profile_prompt(&p, own, 40)) <= 40u);
}

TEST(profile_files_in_repo_load) {
    auto profiles = load_profiles(std::string(WALLET_AI_SOURCE_DIR) + "/prompts/banks");
    for (const auto& p : profiles) {
        CHECK(!p.version.empty());
        CHECK(!p.signature.empty());
        CHECK(p.examples.size() <= 3u);
    }
}

TEST(source_files) {
    CHECK_EQ(extract_file("statement.xlsx", b64("x"), 100).error, std::string("unsupported_file"));
    CHECK_EQ(extract_file("big.csv", b64(std::string(200, 'a')), 100).error, std::string("too_large"));
    CHECK_EQ(extract_file("a.csv", "!!!", 100).error, std::string("bad_base64"));
    auto csv = extract_file("s.csv", b64("\xC4\xE0\xF2\xE0;\xD1\xF3\xEC\xEC\xE0\n"), 1000);
    CHECK(csv.error.empty());
    CHECK(csv.csv);
    CHECK_EQ(csv.text, std::string("Дата;Сумма\n"));
    CHECK_EQ(*decode_base64("0J/RgNC40LLQtdGC"), std::vector<unsigned char>({0xD0, 0x9F, 0xD1, 0x80, 0xD0, 0xB8,
                                                                           0xD0, 0xB2, 0xD0, 0xB5, 0xD1, 0x82}));

    auto pdf = extract_file("s.pdf", b64(tiny_pdf("12.09 KOFE 200,00")), 100000);
    CHECK(pdf.error.empty());
    CHECK(pdf.text.find("KOFE 200,00") != std::string::npos);
    CHECK_EQ(extract_file("scan.pdf", b64(tiny_pdf("")), 100000).error, std::string("no_text"));
}

namespace {

std::string completion(const std::string& content, const std::string& finish = "stop") {
    return nlohmann::json{{"choices", {{{"finish_reason", finish}, {"message", {{"content", content}}}}}},
                          {"usage", {{"completion_tokens", 42}}}}
        .dump();
}

llm::Prompt real(const char* file) {
    return llm::load_prompt(std::string(WALLET_AI_SOURCE_DIR) + "/prompts/" + file);
}

} // namespace

TEST(rows_request_and_response) {
    llm::RowsRequest r;
    r.numbered_text = "17| 12.09 YANDEX*GO MOSCOW -450,00\n";
    r.date_context = "12.09.2026";
    r.today = "2026-10-22";
    r.wallet_currency = "RUB";
    r.profile = "Особенности: последняя колонка — остаток.";
    llm::CallOptions opts;
    opts.max_tokens = llm::rows_max_tokens(1);
    auto body = llm::rows_request_body(r, real("extract_rows.txt"), "wallet-parse", opts);
    const auto& msgs = body["messages"];
    CHECK_EQ(msgs.size(), 2u);
    auto system = msgs[0]["content"].get<std::string>();
    CHECK(system.find("{") == std::string::npos); // все подстановки заменены
    CHECK(system.find("остаток") != std::string::npos);
    CHECK(system.find("не указан") != std::string::npos);
    CHECK(msgs[1]["content"].get<std::string>().find("17| 12.09 YANDEX*GO") != std::string::npos);
    CHECK(msgs[1]["content"].get<std::string>().find("12.09.2026") != std::string::npos);
    CHECK(body["response_format"]["json_schema"]["strict"].get<bool>());
    CHECK_EQ(body["max_tokens"].get<int>(), 260);
    CHECK_EQ(body["reasoning_effort"].get<std::string>(), std::string("none"));

    auto ok = llm::parse_rows_response(200, completion(R"({"rows":[{"lines":[17],"date":"2026-09-12",
        "amount":"450.00","dir":"expense","currency":"","description":"YANDEX*GO MOSCOW"}]})"));
    REQUIRE(ok.status == llm::Status::Ok);
    REQUIRE(ok.rows.size() == 1u);
    CHECK_EQ(ok.rows[0].lines[0], 17);
    CHECK_EQ(ok.rows[0].amount, std::string("450.00"));
    CHECK_EQ(ok.completion_tokens, 42);
    CHECK_EQ(llm::parse_rows_response(200, completion("{\"rows\":[{\"lin", "length")).detail,
             std::string("finish_reason_length"));
    CHECK(llm::parse_rows_response(200, completion(R"({"rows":[{"lines":[1]}]})")).status ==
          llm::Status::BadResponse);
    CHECK(llm::parse_rows_response(503, "").status == llm::Status::Unavailable);

    // Промпт с профилем и фрагментом укладывается в контекст 4096 вместе с ответом.
    llm::RowsRequest big = r;
    big.profile = std::string(500 * 3, 'x');
    big.numbered_text = std::string(1800 - 500 * 3, '1');
    auto big_body = llm::rows_request_body(big, real("extract_rows.txt"), "m", opts);
    std::size_t prompt_tokens = estimate_tokens(big_body["messages"][0]["content"].get<std::string>()) +
                                estimate_tokens(big_body["messages"][1]["content"].get<std::string>());
    CHECK(prompt_tokens + static_cast<std::size_t>(llm::rows_max_tokens(30)) <= 4096u);
}

TEST(classify_request_and_response) {
    llm::ClassifyRequest r{{"YANDEX*GO", "APTEKA 36.6"}, {"Такси", "Здоровье"}};
    auto body = llm::classify_request_body(r, real("classify_rows.txt"), "m", llm::CallOptions{});
    auto schema = body["response_format"]["json_schema"]["schema"];
    CHECK_EQ(schema["properties"]["items"]["items"]["properties"]["category"]["enum"],
             (nlohmann::json{"Такси", "Здоровье", ""}));
    auto user = body["messages"][1]["content"].get<std::string>();
    CHECK_EQ(user, std::string("0| YANDEX*GO\n1| APTEKA 36.6\n"));
    // В модель не уходят суммы и даты — только pattern.
    CHECK(body.dump().find("450") == std::string::npos);
    auto ok = llm::parse_classify_response(200, completion(R"({"items":[{"i":0,"category":"Такси","confidence":0.9}]})"));
    REQUIRE(ok.status == llm::Status::Ok);
    CHECK_EQ(ok.items[0].category, std::string("Такси"));
    CHECK(llm::parse_classify_response(200, completion(R"({"items":[{"i":"0"}]})")).status == llm::Status::BadResponse);
}
