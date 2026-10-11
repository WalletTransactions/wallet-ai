#include "parse_expense.hpp"

#include <cctype>

#include "expense_check.hpp"

using nlohmann::json;

namespace parse_expense {

namespace {

bool matches(const std::string& s, const char* pattern) {
    // pattern: '9' — цифра, остальное — буквально ("9999-99-99").
    std::size_t n = 0;
    for (; pattern[n] != '\0'; ++n) {
        if (n >= s.size()) return false;
        if (pattern[n] == '9' ? std::isdigit(static_cast<unsigned char>(s[n])) == 0 : s[n] != pattern[n]) return false;
    }
    return n == s.size();
}

std::string str(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

const char* category_why(resolver::CategoryPick::Why w) {
    using W = resolver::CategoryPick::Why;
    switch (w) {
    case W::None: return "none";
    case W::Missing: return "missing";
    case W::NotFound: return "not_found";
    case W::Ambiguous: return "ambiguous";
    }
    return "none";
}

const char* wallet_why(resolver::WalletPick::Why w) {
    using W = resolver::WalletPick::Why;
    switch (w) {
    case W::None: return "none";
    case W::NotGiven: return "not_given";
    case W::NotFound: return "not_found";
    case W::Ambiguous: return "ambiguous";
    case W::CurrencyMismatch: return "currency_mismatch";
    case W::NoWalletInCurrency: return "no_wallet_in_currency";
    case W::NoWallets: return "no_wallets";
    }
    return "none";
}

json category_json(const resolver::Category& c) { return {{"uuid", c.uuid}, {"name", c.name}}; }

json wallet_json(const resolver::Wallet& w) {
    return {{"uuid", w.uuid}, {"name", w.name}, {"iso_code", w.iso_code}, {"symbol", w.symbol}, {"precision", w.precision}};
}

} // namespace

std::string validate_input(const json& input) {
    if (!input.is_object()) return "input должен быть объектом";
    std::string text = str(input, "text");
    if (text.empty()) return "input.text обязателен";
    if (text.size() > kMaxTextBytes) return "input.text длиннее " + std::to_string(kMaxTextBytes) + " байт";
    if (!input.contains("now") || !input["now"].is_object()) {
        return "input.now обязателен: {date: YYYY-MM-DD, time: HH:MM:SS} в поясе пользователя";
    }
    if (!matches(str(input["now"], "date"), "9999-99-99")) return "input.now.date — YYYY-MM-DD";
    if (!matches(str(input["now"], "time"), "99:99:99")) return "input.now.time — HH:MM:SS";
    if (input.contains("default_wallet_uuid") && !input["default_wallet_uuid"].is_string()) {
        return "input.default_wallet_uuid — строка";
    }
    return {};
}

json make_job_input(const json& client_input, const json& accounts_body) {
    return {{"text", str(client_input, "text")},
            {"now", client_input["now"]},
            {"default_wallet_uuid", str(client_input, "default_wallet_uuid")},
            {"accounts", accounts_body}};
}

json expense_json(const parser::ParsedExpense& e) {
    return {{"amount", e.amount},         {"fraction_digits", e.fraction_digits}, {"currency_hint", e.currency_hint},
            {"category", e.category},     {"wallet", e.wallet},                   {"datetime", e.datetime},
            {"comment", e.comment}};
}

json resolution_json(const resolver::Resolution& r) {
    using CK = resolver::CategoryPick::Kind;
    using WK = resolver::WalletPick::Kind;
    json category = {{"kind", r.category.kind == CK::Found ? "found" : "ask"}, {"why", category_why(r.category.why)}};
    if (r.category.kind == CK::Found) category["found"] = category_json(r.category.found);
    category["candidates"] = json::array();
    for (const auto& c : r.category.candidates) category["candidates"].push_back(category_json(c));

    const char* wkind = r.wallet.kind == WK::Found ? "found" : (r.wallet.kind == WK::Ask ? "ask" : "reject");
    json wallet = {{"kind", wkind},
                   {"why", wallet_why(r.wallet.why)},
                   {"by_currency", r.wallet.by_currency},
                   {"offer_remember", r.wallet.offer_remember}};
    if (r.wallet.kind == WK::Found) wallet["found"] = wallet_json(r.wallet.found);
    wallet["candidates"] = json::array();
    for (const auto& w : r.wallet.candidates) wallet["candidates"].push_back(wallet_json(w));
    if (r.wallet.force) wallet["force"] = wallet_json(*r.wallet.force);
    return {{"category", category}, {"wallet", wallet}};
}

Outcome run(const json& job_input, llm::Client& client, const llm::CallOptions& opts) {
    Outcome out;
    resolver::Accounts accounts = resolver::parse_accounts(job_input.value("accounts", json::object()));
    parser::Names names = resolver::names_for_parser(accounts);
    parser::Now now{str(job_input["now"], "date"), str(job_input["now"], "time")};
    std::string text = str(job_input, "text");

    llm::ExpenseRequest req;
    req.text = text;
    req.today = now.date;
    for (const auto& c : accounts.categories) req.categories.push_back(c.name);
    for (const auto& w : accounts.wallets) req.wallets.push_back({w.name, w.iso_code});

    llm::Result r = client.extract_expense(req, opts);
    const std::string& prompt_version = client.prompt_version();
    out.log = {{"llm_status", llm::to_string(r.status)},
               {"llm_seconds", r.seconds},
               {"completion_tokens", r.completion_tokens},
               {"reasoning_tokens", r.reasoning_tokens},
               {"prompt_version", prompt_version}};
    if (!r.detail.empty()) out.log["llm_detail"] = r.detail;

    if (r.status == llm::Status::NotExpense) {
        out.status = jobs::Status::Done;
        out.result = {{"outcome", "not_expense"}, {"prompt_version", prompt_version}};
        out.log["outcome"] = "not_expense";
        return out;
    }
    if (r.status != llm::Status::Ok) {
        out.status = jobs::Status::Failed;
        // Для клиента достаточно грубой причины: всё, кроме таймаута и
        // битого ответа, — «ИИ недоступен».
        out.failure = r.status == llm::Status::Timeout       ? "timeout"
                      : r.status == llm::Status::BadResponse ? "bad_response"
                                                             : "unavailable";
        return out;
    }

    out.log["confidence"] = r.extraction.confidence;
    expense_check::Result checked = expense_check::check(r.extraction, text, names, now);
    out.log["notes"] = checked.notes;
    out.status = jobs::Status::Done;
    if (!checked.ok) {
        out.result = {{"outcome", "rejected"}, {"reasons", checked.notes}, {"prompt_version", prompt_version}};
        out.log["outcome"] = "rejected";
        return out;
    }

    resolver::Resolution resolution =
        resolver::resolve(checked.expense, accounts, str(job_input, "default_wallet_uuid"));
    out.result = {{"outcome", "draft"},
                  {"expense", expense_json(checked.expense)},
                  {"resolution", resolution_json(resolution)},
                  {"confidence", r.extraction.confidence},
                  {"prompt_version", prompt_version},
                  {"notes", checked.notes}};
    out.log["outcome"] = "draft";
    return out;
}

} // namespace parse_expense
