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
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "llm_client.hpp"
#include "nlohmann/json.hpp"

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
        } else {
            texts.push_back(a);
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
