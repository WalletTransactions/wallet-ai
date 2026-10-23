// Ручная проверка разбора траты против живой LLM — без сервиса, токенов и
// справочника из wallet-report. Для подбора промпта и модели; корпус и
// eval — шаг 10 плана бота.
//
//   LLM_URL=http://127.0.0.1:1234/v1 LLM_MODEL=wallet-parse \
//     wallet_ai_llm_try --categories "Кафе,Продукты,Такси" \
//       --wallets "Тинькофф:RUB,Revolut:EUR" --today 2026-10-11 \
//       [--reasoning none] [--max-tokens 512] [--prompt prompts/expense.txt] \
//       "вчера на такси 450 с тинькоффа" ...
//
// --prompt — другой файл промпта (подбор версий без правки prompts/).
//
// Печатает по строке JSON на фразу: статус, поля, время, токены.
//
// Выписка (импорт, extract_rows + проверка кодом):
//   wallet_ai_llm_try --statement tests/statements/<bank>/sample.csv \
//     [--profile prompts/banks/<bank>.txt] [--currency RUB] [--today 2026-10-22] \
//     [--chunk-chars 1800] [--rows]
//   Окно модели — LLM_CONTEXT (по умолчанию 8192, как в scripts/lms-load.sh).
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "llm_client.hpp"
#include "nlohmann/json.hpp"
#include "statement/chunk.hpp"
#include "statement/profiles.hpp"
#include "statement/source.hpp"
#include "statement/textutil.hpp"
#include "statement/verify.hpp"

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

// --statement: выписка целиком тем же путём, что задания extract_rows
// (фрагменты, промпт, профиль, проверка кодом), без сервиса и базы. По
// строке JSON на фрагмент и итог — доли verified / check_failed / unparsed
// (openspec/changes/add-statement-import, задачи 4.2 и 4.11).
int run_statement(const llm::Config& cfg, const std::string& path, const std::string& today,
                  const std::string& currency, const std::string& profile_path, std::size_t chunk_chars,
                  int context, bool print_rows) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "нет файла " << path << "\n";
        return 2;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string text = buf.str();
    auto ext = path.substr(path.find_last_of('.') + 1);
    if (ext == "pdf") {
        auto t = stmt::pdf_to_text(text, 20, 256u * 1024 * 1024);
        if (!t) {
            std::cerr << "pdftotext не справился\n";
            return 2;
        }
        text = *t;
    } else {
        text = stmt::decode_text(text);
    }
    std::replace(text.begin(), text.end(), '\f', '\n');
    auto lines = stmt::split_lines(text);

    llm::StatementPrompts prompts{llm::load_prompt("prompts/extract_rows.txt"),
                                  llm::load_prompt("prompts/classify_rows.txt")};
    auto client = llm::make_http_client(cfg, llm::load_prompt("prompts/expense.txt"), prompts);
    std::string profile_text;
    stmt::CheckRules rules;
    std::string profile_version = "none";
    std::optional<std::vector<stmt::ColumnRow>> by_columns;
    if (!profile_path.empty()) {
        std::ifstream pf(profile_path, std::ios::binary);
        std::ostringstream pbuf;
        pbuf << pf.rdbuf();
        auto p = stmt::parse_profile(pbuf.str());
        profile_text = stmt::profile_prompt(&p, {}, 500);
        rules = p.check;
        profile_version = p.version;
        if (ext == "csv") by_columns = stmt::parse_by_columns(p, lines);
    }
    std::size_t profile_tokens = stmt::estimate_tokens(profile_text);
    auto plans = stmt::plan_chunks(lines, llm::extract_chunk_chars(chunk_chars, 600, context, profile_tokens));
    // CSV по колонкам профиля (D11) — без LLM и без фрагментов.
    if (by_columns) plans.clear();
    auto period = stmt::find_period(lines);

    int verified = 0;
    int check_failed = 0;
    std::set<int> covered;
    auto report = [&](const stmt::Verified& v) {
        if (print_rows) {
            std::cout << nlohmann::json{{"lines", v.lines}, {"date", v.date}, {"amount", v.amount},
                                        {"dir", v.dir}, {"description", v.description}, {"check", v.check}}
                             .dump(-1, ' ', false)
                      << "\n";
        }
    };
    if (by_columns) {
        stmt::VerifyContext ctx;
        ctx.lines = &lines;
        ctx.chunk_first = 1;
        ctx.chunk_last = static_cast<int>(lines.size());
        ctx.today = today;
        ctx.period = period;
        ctx.wallet_currency = currency;
        ctx.rules = rules;
        for (const auto& r : *by_columns) {
            auto v = stmt::verify_row(r.candidate, ctx);
            if (!v) continue;
            if (v->check.empty()) {
                ++verified;
                covered.insert(v->lines.begin(), v->lines.end());
            } else {
                ++check_failed;
            }
            report(*v);
        }
    }
    for (const auto& c : plans) {
        llm::RowsRequest req;
        req.numbered_text = stmt::numbered_text(lines, c.first_line, c.last_line);
        req.date_context = c.date_context;
        req.today = today;
        req.period = period.from.empty() ? "" : period.from + " — " + period.to;
        req.wallet_currency = currency;
        req.profile = profile_text;
        llm::CallOptions opts;
        std::size_t chars = stmt::utf8_length(req.numbered_text);
        opts.max_tokens = llm::rows_max_tokens(stmt::money_lines(lines, c.first_line, c.last_line), chars,
                                               llm::rows_answer_room(context, profile_tokens, (chars + 2) / 3));
        opts.timeout_ms = 180000;
        auto r = client->extract_rows(req, opts);
        stmt::VerifyContext ctx;
        ctx.lines = &lines;
        ctx.chunk_first = c.first_line;
        ctx.chunk_last = c.last_line;
        ctx.date_context = c.date_context;
        ctx.today = today;
        ctx.period = period;
        ctx.wallet_currency = currency;
        ctx.rules = rules;
        int ok = 0;
        int bad = 0;
        for (const auto& row : r.rows) {
            auto v = stmt::verify_row({row.lines, row.date, row.amount, row.dir, row.currency, row.description}, ctx);
            if (!v) continue;
            if (v->check.empty()) {
                ++ok;
                covered.insert(v->lines.begin(), v->lines.end());
            } else {
                ++bad;
            }
            report(*v);
        }
        verified += ok;
        check_failed += bad;
        nlohmann::json out = {{"chunk", std::to_string(c.first_line) + "-" + std::to_string(c.last_line)},
                              {"status", llm::to_string(r.status)},
                              {"seconds", static_cast<int>(r.seconds * 10) / 10.0},
                              {"tokens", r.completion_tokens},
                              {"verified", ok},
                              {"check_failed", bad}};
        if (!r.detail.empty()) out["detail"] = r.detail;
        std::cout << out.dump(-1, ' ', false) << "\n";
    }
    int unparsed = static_cast<int>(
        stmt::uncovered_lines(lines, 1, static_cast<int>(lines.size()), covered, rules.stop_words).size());
    int total = verified + check_failed + unparsed;
    auto share = [&](int n) { return total == 0 ? 0.0 : static_cast<int>(1000.0 * n / total) / 10.0; };
    std::cout << nlohmann::json{{"profile", profile_version},     {"chunks", plans.size()},
                                {"by_columns", by_columns.has_value()},
                                {"verified", verified},           {"check_failed", check_failed},
                                {"unparsed", unparsed},           {"verified_pct", share(verified)},
                                {"check_failed_pct", share(check_failed)}, {"unparsed_pct", share(unparsed)}}
                     .dump(-1, ' ', false)
              << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    llm::Config cfg;
    cfg.url = env_or("LLM_URL", "http://127.0.0.1:1234/v1");
    cfg.model = env_or("LLM_MODEL", "wallet-parse");
    cfg.api_key = env_or("LLM_API_KEY", "");

    llm::ExpenseRequest base;
    base.today = "2026-10-11";
    llm::CallOptions opts;
    std::vector<std::string> texts;
    bool dump_body = false; // --dump-body: напечатать тело запроса первой фразы и выйти
    std::string prompt_path = "prompts/expense.txt";
    std::string statement_path;
    std::string currency = "RUB";
    std::string profile_path;
    std::size_t chunk_chars = 1800;
    bool print_rows = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--categories") {
            base.categories = split(next(), ',');
        } else if (a == "--wallets") {
            for (const auto& w : split(next(), ',')) {
                auto parts = split(w, ':');
                base.wallets.push_back({parts.empty() ? w : parts[0], parts.size() > 1 ? parts[1] : ""});
            }
        } else if (a == "--today") {
            base.today = next();
        } else if (a == "--reasoning") {
            opts.reasoning_effort = next();
        } else if (a == "--max-tokens") {
            opts.max_tokens = std::stoi(next());
        } else if (a == "--timeout-ms") {
            opts.timeout_ms = std::stoi(next());
        } else if (a == "--prompt") {
            prompt_path = next();
        } else if (a == "--dump-body") {
            dump_body = true;
        } else if (a == "--statement") {
            statement_path = next();
        } else if (a == "--currency") {
            currency = next();
        } else if (a == "--profile") {
            profile_path = next();
        } else if (a == "--chunk-chars") {
            chunk_chars = static_cast<std::size_t>(std::stoi(next()));
        } else if (a == "--rows") {
            print_rows = true;
        } else {
            texts.push_back(a);
        }
    }
    if (!statement_path.empty()) {
        try {
            return run_statement(cfg, statement_path, base.today, currency, profile_path, chunk_chars,
                                 std::stoi(env_or("LLM_CONTEXT", "8192")), print_rows);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 2;
        }
    }
    if (texts.empty()) {
        std::cerr << "usage: wallet_ai_llm_try --categories A,B --wallets Name:ISO,... [--reasoning none] text...\n";
        return 2;
    }

    llm::Prompt prompt;
    try {
        prompt = llm::load_prompt(prompt_path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }

    if (dump_body) {
        llm::ExpenseRequest req = base;
        req.text = texts.front();
        std::cout << llm::expense_request_body(req, prompt, cfg.model, opts).dump(2) << "\n";
        return 0;
    }

    auto client = llm::make_http_client(cfg, prompt);
    int failed = 0;
    for (const auto& t : texts) {
        llm::ExpenseRequest req = base;
        req.text = t;
        llm::Result r = client->extract_expense(req, opts);
        nlohmann::json out = {{"text", t},
                              {"status", llm::to_string(r.status)},
                              {"seconds", static_cast<int>(r.seconds * 10) / 10.0},
                              {"tokens", r.completion_tokens},
                              {"reasoning_tokens", r.reasoning_tokens}};
        if (!r.detail.empty()) out["detail"] = r.detail;
        if (r.status == llm::Status::Ok || r.status == llm::Status::NotExpense) {
            const auto& e = r.extraction;
            out["amount"] = e.amount;
            out["currency"] = e.currency;
            out["category"] = e.category;
            out["wallet"] = e.wallet;
            out["date"] = e.date;
            out["comment"] = e.comment;
            out["confidence"] = e.confidence;
        } else {
            ++failed;
        }
        std::cout << out.dump(-1, ' ', false) << "\n";
    }
    std::cout << "prompt " << prompt.version << ", reasoning "
              << (opts.reasoning_effort.empty() ? "(default)" : opts.reasoning_effort) << "\n";
    return failed == 0 ? 0 : 1;
}
