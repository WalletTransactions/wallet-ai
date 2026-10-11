// Задание parse_expense (org/docs/TODO_wallet_ai.md, §3): трата свободным
// текстом -> проверенный черновик. Вход клиента:
//
//   {"text": "вчера такси 450 с тинькоффа",
//    "now": {"date": "2026-10-11", "time": "14:05:00"},   // в поясе пользователя
//    "default_wallet_uuid": "…"}                          // необязательно
//
// Справочник счетов (GET /api/reports/accounts) берётся токеном
// пользователя в момент СОЗДАНИЯ задания и кладётся во вход: к запуску
// короткий токен бота мог истечь, а хранить токен в задании незачем.
//
// Результат (status done):
//   {"outcome": "draft", "expense": {...}, "resolution": {...}, "confidence", "prompt_version", "notes": [...]}
//   {"outcome": "not_expense", "prompt_version"}
//   {"outcome": "rejected", "reasons": ["amount_not_in_text", ...], "prompt_version"}
// LLM недоступна, таймаут, битый ответ — status failed, failure = причина.
#pragma once

#include <string>

#include "expense/resolver.hpp"
#include "jobs.hpp"
#include "llm_client.hpp"
#include "nlohmann/json.hpp"

namespace parse_expense {

inline constexpr const char* kKind = "parse_expense";
// Длиннее — не трата, а чужой текст (и лимит на промпт).
inline constexpr std::size_t kMaxTextBytes = 500;

// Пусто — вход годится; иначе текст ошибки для 400.
std::string validate_input(const nlohmann::json& input);

// Вход задания: что прислал клиент + снимок справочника.
nlohmann::json make_job_input(const nlohmann::json& client_input, const nlohmann::json& accounts_body);

struct Outcome {
    jobs::Status status = jobs::Status::Failed;
    nlohmann::json result;
    std::string failure;
    // Для лога: исход, причины, токены, время LLM — без текста и сумм.
    nlohmann::json log;
};

Outcome run(const nlohmann::json& job_input, llm::Client& client, const llm::CallOptions& opts);

nlohmann::json expense_json(const parser::ParsedExpense& e);
nlohmann::json resolution_json(const resolver::Resolution& r);

} // namespace parse_expense
