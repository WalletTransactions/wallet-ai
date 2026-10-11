// Состояние процесса wallet-ai. Базы нет (TODO_wallet_ai.md, §6): задания
// живут в памяти (jobs.hpp), база появится вместе с импортом выписок.
#pragma once

#include <atomic>
#include <memory>

#include "clients/reports_client.hpp"
#include "jobs.hpp"
#include "llm_client.hpp"
#include "llm_probe.hpp"
#include "util/jwks.hpp"

struct AppState {
    // Публичные ключи google-auth — проверка подписи access-токена.
    std::unique_ptr<jwks::Cache> jwks;

    llm::Config llm;

    // System-промпт разбора траты — prompts/expense.txt, читается при старте.
    llm::Prompt expense_prompt;

    // Последний результат пробы LLM (фоновый поток в main.cpp, раз в
    // LLM_PROBE_INTERVAL_SECS). Читается хендлерами из потоков httplib —
    // atomic, мьютекс ради одного enum не нужен.
    std::atomic<llm::ProbeResult> llm_status{llm::ProbeResult::Disabled};

    // Справочник счетов пользователя — его токеном, при создании задания.
    std::unique_ptr<reports::ReportsApi> reports;

    // Клиент LLM для воркеров (один на процесс: состояния у него нет).
    std::unique_ptr<llm::Client> llm_client;

    // Настройки вызова на вид задания (§3, §5): сколько думать, max_tokens,
    // дедлайн генерации.
    llm::CallOptions parse_expense_opts;

    std::unique_ptr<jobs::Store> jobs;
};
