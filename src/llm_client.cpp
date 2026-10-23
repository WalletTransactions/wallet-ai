#include "llm_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "httplib.h"

using nlohmann::json;

namespace llm {

namespace {

// Дни от 1970-01-01 и обратно — H. Hinnant, "chrono-Compatible Low-Level Date
// Algorithms". <chrono> C++17 календаря не знает.
long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
}

void civil_from_days(long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (m <= 2);
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& s : items) {
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out;
}

json string_enum(const std::vector<std::string>& names) {
    json values = json::array();
    for (const auto& n : names) {
        if (!n.empty() && std::find(values.begin(), values.end(), n) == values.end()) values.push_back(n);
    }
    values.push_back(""); // «не знаю» — всегда допустимо
    return {{"type", "string"}, {"enum", values}};
}

json expense_schema(const ExpenseRequest& r) {
    std::vector<std::string> wallet_names;
    for (const auto& w : r.wallets) wallet_names.push_back(w.first);
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"amount", "currency", "category", "wallet", "date", "comment", "confidence"}},
        {"properties",
         {
             {"amount", {{"type", "string"}, {"pattern", "^([0-9]+([.,][0-9]+)?)?$"}}},
             {"currency", {{"type", "string"}, {"pattern", "^([A-Z]{3})?$"}}},
             // enum, а не просьба в промпте: llama.cpp превращает схему в
             // грамматику, и модель физически не может вернуть чужое имя.
             {"category", string_enum(r.categories)},
             {"wallet", string_enum(wallet_names)},
             {"date", {{"type", "string"}, {"pattern", "^([0-9]{4}-[0-9]{2}-[0-9]{2})?$"}}},
             {"comment", {{"type", "string"}}},
             {"confidence", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
         }},
    };
}

// «вчера (суббота) 2026-10-10, позавчера (пятница) 2026-10-09, четверг
// 2026-10-08, ...» — 7 прошедших дней: «в пятницу» модель берёт из списка.
std::string recent_days(const std::string& today) {
    std::string out;
    for (int i = 1; i <= 7; ++i) {
        std::string day = add_days(today, -i);
        if (day.empty()) return {};
        std::string label = weekday_ru(day);
        if (i == 1) label = "вчера (" + label + ")";
        if (i == 2) label = "позавчера (" + label + ")";
        if (!out.empty()) out += ", ";
        out += label + " " + day;
    }
    return out;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
        s.replace(pos, from.size(), to);
    }
}

json shot_answer(const std::string& amount, const std::string& currency, const std::string& category,
                 const std::string& wallet, const std::string& date, const std::string& comment, double confidence) {
    return {{"amount", amount},     {"currency", currency}, {"category", category}, {"wallet", wallet},
            {"date", date},         {"comment", comment},   {"confidence", confidence}};
}

// Примеры строятся из имён ЭТОГО пользователя: ответ примера обязан
// проходить ту же схему (enum), иначе модель учится на невозможном ответе.
// Смысловое сопоставление («кофе» → «Кафе») — из system, примеры учат форме:
// сумма без пробелов, относительная дата, кошелёк, «не трата».
std::vector<std::pair<std::string, json>> shots(const ExpenseRequest& r) {
    std::string cat0 = r.categories.empty() ? "" : r.categories[0];
    std::string cat1 = r.categories.size() > 1 ? r.categories[1] : cat0;
    std::string wallet0 = r.wallets.empty() ? "" : r.wallets[0].first;
    std::string two_days_ago = add_days(r.today, -2);

    std::vector<std::pair<std::string, json>> out;
    out.push_back({(cat0.empty() ? std::string("покупка") : cat0) + " 200",
                   shot_answer("200", "", cat0, "", "", cat0.empty() ? "покупка" : "", 0.9)});
    std::string text2 = "позавчера 1 340,50 " + (cat1.empty() ? std::string("разное") : cat1) +
                        (wallet0.empty() ? "" : " с " + wallet0);
    out.push_back({text2, shot_answer("1340,50", "", cat1, wallet0, two_days_ago, cat1.empty() ? "разное" : "", 0.9)});
    out.push_back({"привет, как дела", shot_answer("", "", "", "", "", "", 0.9)});
    return out;
}

bool is_string_field(const json& j, const char* key) { return j.contains(key) && j.at(key).is_string(); }

} // namespace

const char* to_string(Status s) {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::NotExpense: return "not_expense";
    case Status::Timeout: return "timeout";
    case Status::Unavailable: return "unavailable";
    case Status::Unauthorized: return "unauthorized";
    case Status::ModelNotLoaded: return "model_not_loaded";
    case Status::BadResponse: return "bad_response";
    }
    return "unknown";
}

std::string add_days(const std::string& ymd, int days) {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (ymd.size() != 10 || std::sscanf(ymd.c_str(), "%4d-%2u-%2u", &y, &m, &d) != 3) return {};
    if (m < 1 || m > 12 || d < 1 || d > 31) return {};
    civil_from_days(days_from_civil(y, m, d) + days, y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return buf;
}

std::string weekday_ru(const std::string& ymd) {
    static const char* kNames[] = {"четверг", "пятница", "суббота", "воскресенье",
                                   "понедельник", "вторник", "среда"}; // 1970-01-01 — четверг
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (ymd.size() != 10 || std::sscanf(ymd.c_str(), "%4d-%2u-%2u", &y, &m, &d) != 3) return {};
    if (m < 1 || m > 12 || d < 1 || d > 31) return {};
    long days = days_from_civil(y, m, d);
    return kNames[((days % 7) + 7) % 7];
}

std::string strip_think(const std::string& content) {
    std::string s = content;
    auto end = s.find("</think>");
    if (s.find("<think>") != std::string::npos && end != std::string::npos) s = s.substr(end + 8);
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

Prompt parse_prompt(const std::string& content) {
    static const std::string kVersionTag = "# version:";
    Prompt p;
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(kVersionTag, 0) == 0) {
            p.version = strip_think(line.substr(kVersionTag.size())); // strip_think заодно обрезает пробелы
            continue;
        }
        if (!line.empty() && line[0] == '#') continue;
        if (!p.text.empty()) p.text += "\n";
        p.text += line;
    }
    p.text = strip_think(p.text);
    if (p.version.empty()) throw std::runtime_error("в промпте нет строки \"# version: ...\"");
    if (p.text.empty()) throw std::runtime_error("промпт пустой");
    return p;
}

Prompt load_prompt(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("не найден файл промпта: " + path);
    std::ostringstream buf;
    buf << f.rdbuf();
    try {
        return parse_prompt(buf.str());
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

std::string render_system_prompt(const Prompt& prompt, const ExpenseRequest& r) {
    std::string wallets;
    for (const auto& [name, iso] : r.wallets) {
        if (!wallets.empty()) wallets += ", ";
        wallets += name + " (" + iso + ")";
    }
    std::string s = prompt.text;
    replace_all(s, "{today}", r.today);
    replace_all(s, "{weekday}", weekday_ru(r.today));
    replace_all(s, "{recent_days}", recent_days(r.today));
    replace_all(s, "{year}", r.today.size() >= 4 ? r.today.substr(0, 4) : "");
    replace_all(s, "{categories}", r.categories.empty() ? "(нет)" : join(r.categories));
    replace_all(s, "{wallets}", wallets.empty() ? "(нет)" : wallets);
    return s;
}

json expense_request_body(const ExpenseRequest& r, const Prompt& prompt, const std::string& model,
                          const CallOptions& opts) {
    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", render_system_prompt(prompt, r)}});
    for (const auto& [text, answer] : shots(r)) {
        messages.push_back({{"role", "user"}, {"content", text}});
        messages.push_back({{"role", "assistant"}, {"content", answer.dump()}});
    }
    messages.push_back({{"role", "user"}, {"content", r.text}});

    json body = {
        {"model", model},
        {"temperature", 0},
        {"max_tokens", opts.max_tokens},
        {"stream", false},
        {"messages", messages},
        {"response_format",
         {{"type", "json_schema"}, {"json_schema", {{"name", "expense"}, {"strict", true}, {"schema", expense_schema(r)}}}}},
    };
    if (!opts.reasoning_effort.empty()) body["reasoning_effort"] = opts.reasoning_effort;
    return body;
}

Result parse_expense_response(int http_status, const std::string& body) {
    Result r;
    if (http_status == 401 || http_status == 403) {
        r.status = Status::Unauthorized;
        return r;
    }
    if (http_status == 404) {
        r.status = Status::ModelNotLoaded;
        return r;
    }
    if (http_status == 400) {
        r.status = Status::BadResponse;
        r.detail = "http_400";
        return r;
    }
    if (http_status < 200 || http_status >= 300) {
        r.status = Status::Unavailable;
        r.detail = "http_" + std::to_string(http_status);
        return r;
    }

    r.status = Status::BadResponse;
    json resp;
    try {
        resp = json::parse(body);
    } catch (const std::exception&) {
        r.detail = "response_not_json";
        return r;
    }
    if (resp.contains("usage") && resp["usage"].is_object()) {
        const auto& u = resp["usage"];
        r.completion_tokens = u.value("completion_tokens", 0);
        if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
            r.reasoning_tokens = u["completion_tokens_details"].value("reasoning_tokens", 0);
        }
    }
    if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty()) {
        r.detail = "no_choices";
        return r;
    }
    const auto& choice = resp["choices"][0];
    if (choice.value("finish_reason", std::string{}) == "length") {
        r.detail = "finish_reason_length"; // JSON оборван — max_tokens съели рассуждения?
        return r;
    }
    if (!choice.contains("message") || !choice["message"].contains("content") || !choice["message"]["content"].is_string()) {
        r.detail = "no_content";
        return r;
    }
    // reasoning_content (так LM Studio отдаёт рассуждения) не нужен.
    json e;
    try {
        e = json::parse(strip_think(choice["message"]["content"].get<std::string>()));
    } catch (const std::exception&) {
        r.detail = "content_not_json";
        return r;
    }
    for (const char* key : {"amount", "currency", "category", "wallet", "date", "comment"}) {
        if (!is_string_field(e, key)) {
            r.detail = std::string("schema_") + key;
            return r;
        }
    }
    if (!e.contains("confidence") || !e["confidence"].is_number()) {
        r.detail = "schema_confidence";
        return r;
    }

    r.extraction.amount = e["amount"].get<std::string>();
    r.extraction.currency = e["currency"].get<std::string>();
    r.extraction.category = e["category"].get<std::string>();
    r.extraction.wallet = e["wallet"].get<std::string>();
    r.extraction.date = e["date"].get<std::string>();
    r.extraction.comment = e["comment"].get<std::string>();
    r.extraction.confidence = e["confidence"].get<double>();
    r.status = r.extraction.amount.empty() ? Status::NotExpense : Status::Ok;
    return r;
}

namespace {

// Общая часть ответа chat/completions: HTTP-статус, choices, обрыв,
// <think>, JSON. content заполняется только при Status::Ok.
struct Completion {
    Status status = Status::BadResponse;
    std::string detail;
    json content;
    int completion_tokens = 0;
    int reasoning_tokens = 0;
};

Completion parse_completion(int http_status, const std::string& body) {
    Completion c;
    if (http_status == 401 || http_status == 403) {
        c.status = Status::Unauthorized;
        return c;
    }
    if (http_status == 404) {
        c.status = Status::ModelNotLoaded;
        return c;
    }
    if (http_status == 400) {
        c.detail = "http_400";
        return c;
    }
    if (http_status < 200 || http_status >= 300) {
        c.status = Status::Unavailable;
        c.detail = "http_" + std::to_string(http_status);
        return c;
    }
    json resp;
    try {
        resp = json::parse(body);
    } catch (const std::exception&) {
        c.detail = "response_not_json";
        return c;
    }
    if (resp.contains("usage") && resp["usage"].is_object()) {
        const auto& u = resp["usage"];
        c.completion_tokens = u.value("completion_tokens", 0);
        if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
            c.reasoning_tokens = u["completion_tokens_details"].value("reasoning_tokens", 0);
        }
    }
    if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty()) {
        c.detail = "no_choices";
        return c;
    }
    const auto& choice = resp["choices"][0];
    if (choice.value("finish_reason", std::string{}) == "length") {
        c.detail = "finish_reason_length";
        return c;
    }
    if (!choice.contains("message") || !choice["message"].contains("content") || !choice["message"]["content"].is_string()) {
        c.detail = "no_content";
        return c;
    }
    try {
        c.content = json::parse(strip_think(choice["message"]["content"].get<std::string>()));
    } catch (const std::exception&) {
        c.detail = "content_not_json";
        return c;
    }
    c.status = Status::Ok;
    return c;
}

json rows_schema() {
    json row = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"lines", "date", "amount", "dir", "currency", "description"}},
        {"properties",
         {
             {"lines", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 1}}}, {"minItems", 1}}},
             {"date", {{"type", "string"}, {"pattern", "^([0-9]{4}-[0-9]{2}-[0-9]{2})?$"}}},
             {"amount", {{"type", "string"}, {"pattern", "^[0-9]+([.][0-9]{1,2})?$"}}},
             {"dir", {{"type", "string"}, {"enum", {"expense", "income", "transfer"}}}},
             {"currency", {{"type", "string"}, {"pattern", "^([A-Z]{3})?$"}}},
             {"description", {{"type", "string"}}},
         }},
    };
    return {{"type", "object"},
            {"additionalProperties", false},
            {"required", {"rows"}},
            {"properties", {{"rows", {{"type", "array"}, {"items", row}}}}}};
}

json classify_schema(const std::vector<std::string>& categories) {
    json item = {{"type", "object"},
                 {"additionalProperties", false},
                 {"required", {"i", "category", "confidence"}},
                 {"properties",
                  {{"i", {{"type", "integer"}, {"minimum", 0}}},
                   {"category", string_enum(categories)},
                   {"confidence", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}}};
    return {{"type", "object"},
            {"additionalProperties", false},
            {"required", {"items"}},
            {"properties", {{"items", {{"type", "array"}, {"items", item}}}}}};
}

json chat_body(const std::string& model, const CallOptions& opts, const std::string& system, const std::string& user,
               const char* schema_name, json schema) {
    json body = {
        {"model", model},
        {"temperature", 0},
        {"max_tokens", opts.max_tokens},
        {"stream", false},
        {"messages", json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}})},
        {"response_format",
         {{"type", "json_schema"}, {"json_schema", {{"name", schema_name}, {"strict", true}, {"schema", std::move(schema)}}}}},
    };
    if (!opts.reasoning_effort.empty()) body["reasoning_effort"] = opts.reasoning_effort;
    return body;
}

} // namespace

int rows_answer_room(int context, std::size_t profile_tokens, std::size_t chunk_tokens) {
    long room = static_cast<long>(context) - kRowsSystemTokens - static_cast<long>(profile_tokens) -
                static_cast<long>(chunk_tokens) - 64;
    return static_cast<int>(std::max(room, 300L));
}

int rows_max_tokens(int money_lines, std::size_t chunk_chars, int answer_room) {
    std::size_t n = 200 + 100 * static_cast<std::size_t>(std::max(money_lines, 1)) + chunk_chars / 6;
    return static_cast<int>(std::min<std::size_t>(n, static_cast<std::size_t>(std::max(answer_room, 300))));
}

std::size_t extract_chunk_chars(std::size_t configured, std::size_t min_chars, int context,
                                std::size_t profile_tokens) {
    // Фрагмент из C символов — C/3 токенов промпта и до 2C/3 токенов ответа.
    long free_tokens = static_cast<long>(context) - kRowsSystemTokens - static_cast<long>(profile_tokens) - 64;
    std::size_t fit = free_tokens > 0 ? static_cast<std::size_t>(free_tokens) : 0;
    return std::max(std::min(configured, fit), min_chars);
}

json rows_request_body(const RowsRequest& r, const Prompt& prompt, const std::string& model, const CallOptions& opts) {
    std::string system = prompt.text;
    replace_all(system, "{today}", r.today);
    replace_all(system, "{period}", r.period.empty() ? "не указан" : r.period);
    replace_all(system, "{currency}", r.wallet_currency);
    replace_all(system, "{profile}", r.profile);
    std::string user;
    if (!r.date_context.empty()) user += "Текущая дата (заголовок выше фрагмента): " + r.date_context + "\n";
    user += r.numbered_text;
    return chat_body(model, opts, strip_think(system), user, "statement_rows", rows_schema());
}

RowsResult parse_rows_response(int http_status, const std::string& body) {
    RowsResult r;
    Completion c = parse_completion(http_status, body);
    r.status = c.status;
    r.detail = c.detail;
    r.completion_tokens = c.completion_tokens;
    r.reasoning_tokens = c.reasoning_tokens;
    if (c.status != Status::Ok) return r;
    if (!c.content.is_object() || !c.content.contains("rows") || !c.content["rows"].is_array()) {
        r.status = Status::BadResponse;
        r.detail = "schema_rows";
        return r;
    }
    for (const auto& item : c.content["rows"]) {
        if (!item.is_object() || !item.contains("lines") || !item["lines"].is_array()) {
            r.status = Status::BadResponse;
            r.detail = "schema_lines";
            r.rows.clear();
            return r;
        }
        ExtractedRow row;
        for (const auto& n : item["lines"]) {
            if (n.is_number_integer()) row.lines.push_back(n.get<int>());
        }
        for (auto [key, field] : {std::pair<const char*, std::string*>{"date", &row.date}, {"amount", &row.amount},
                                  {"dir", &row.dir}, {"currency", &row.currency}, {"description", &row.description}}) {
            if (!is_string_field(item, key)) {
                r.status = Status::BadResponse;
                r.detail = std::string("schema_") + key;
                r.rows.clear();
                return r;
            }
            *field = item[key].get<std::string>();
        }
        r.rows.push_back(std::move(row));
    }
    return r;
}

json classify_request_body(const ClassifyRequest& r, const Prompt& prompt, const std::string& model,
                           const CallOptions& opts) {
    std::string system = prompt.text;
    replace_all(system, "{categories}", r.categories.empty() ? "(нет)" : join(r.categories));
    std::string user;
    for (std::size_t i = 0; i < r.patterns.size(); ++i) user += std::to_string(i) + "| " + r.patterns[i] + "\n";
    return chat_body(model, opts, system, user, "statement_categories", classify_schema(r.categories));
}

ClassifyResult parse_classify_response(int http_status, const std::string& body) {
    ClassifyResult r;
    Completion c = parse_completion(http_status, body);
    r.status = c.status;
    r.detail = c.detail;
    r.completion_tokens = c.completion_tokens;
    r.reasoning_tokens = c.reasoning_tokens;
    if (c.status != Status::Ok) return r;
    if (!c.content.is_object() || !c.content.contains("items") || !c.content["items"].is_array()) {
        r.status = Status::BadResponse;
        r.detail = "schema_items";
        return r;
    }
    for (const auto& item : c.content["items"]) {
        if (!item.is_object() || !item.contains("i") || !item["i"].is_number_integer() ||
            !is_string_field(item, "category") || !item.contains("confidence") || !item["confidence"].is_number()) {
            r.status = Status::BadResponse;
            r.detail = "schema_item";
            r.items.clear();
            return r;
        }
        r.items.push_back({item["i"].get<int>(), item["category"].get<std::string>(), item["confidence"].get<double>()});
    }
    return r;
}

namespace {

class HttpClient : public Client {
public:
    HttpClient(Config cfg, Prompt prompt, StatementPrompts statement)
        : cfg_(std::move(cfg)),
          prompt_(std::move(prompt)),
          statement_(std::move(statement)),
          base_(split_base_url(cfg_.url)) {}

    const std::string& prompt_version() const override { return prompt_.version; }

    Result extract_expense(const ExpenseRequest& req, const CallOptions& opts) override {
        auto started = std::chrono::steady_clock::now();
        Result r;
        auto res = post(expense_request_body(req, prompt_, cfg_.model, opts), opts, r.status, r.detail);
        if (res) r = parse_expense_response(res->status, res->body);
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return r;
    }

    RowsResult extract_rows(const RowsRequest& req, const CallOptions& opts) override {
        auto started = std::chrono::steady_clock::now();
        RowsResult r;
        if (statement_.rows.text.empty()) {
            r.detail = "no_prompt";
            return r;
        }
        auto res = post(rows_request_body(req, statement_.rows, cfg_.model, opts), opts, r.status, r.detail);
        if (res) r = parse_rows_response(res->status, res->body);
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return r;
    }

    ClassifyResult classify_rows(const ClassifyRequest& req, const CallOptions& opts) override {
        auto started = std::chrono::steady_clock::now();
        ClassifyResult r;
        if (statement_.classify.text.empty()) {
            r.detail = "no_prompt";
            return r;
        }
        auto res = post(classify_request_body(req, statement_.classify, cfg_.model, opts), opts, r.status, r.detail);
        if (res) r = parse_classify_response(res->status, res->body);
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return r;
    }

private:
    // nullopt — до HTTP-ответа не дошло; status/detail тогда заполнены.
    std::optional<httplib::Response> post(const json& body, const CallOptions& opts, Status& status,
                                          std::string& detail) {
        if (cfg_.url.empty() || base_.origin.empty()) {
            status = Status::Unavailable;
            detail = "llm_disabled";
            return std::nullopt;
        }
        // Клиент на вызов: вызовы редкие и долгие, держать соединение незачем.
        httplib::Client cli(base_.origin);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(opts.timeout_ms / 1000, (opts.timeout_ms % 1000) * 1000);
        cli.set_max_timeout(opts.timeout_ms); // дедлайн на весь запрос
        httplib::Headers headers;
        if (!cfg_.api_key.empty()) headers.emplace("Authorization", "Bearer " + cfg_.api_key);

        auto res = cli.Post(base_.path + "/chat/completions", headers, body.dump(), "application/json");
        if (!res) {
            auto err = res.error();
            status = (err == httplib::Error::Read || err == httplib::Error::Timeout) ? Status::Timeout
                                                                                     : Status::Unavailable;
            detail = httplib::to_string(err);
            return std::nullopt;
        }
        return *res;
    }

    Config cfg_;
    Prompt prompt_;
    StatementPrompts statement_;
    BaseUrl base_;
};

} // namespace

std::unique_ptr<Client> make_http_client(const Config& cfg, Prompt prompt, StatementPrompts statement) {
    return std::make_unique<HttpClient>(cfg, std::move(prompt), std::move(statement));
}

} // namespace llm
