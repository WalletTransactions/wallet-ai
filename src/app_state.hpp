// Состояние процесса wallet-ai. Базы на шаге 1 нет (TODO_wallet_ai.md, §6):
// появится вместе с импортом выписок.
#pragma once

#include <atomic>
#include <memory>

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
    // LLM_PROBE_INTERVAL_SECS). Читается хендлером /api/ai/status из потоков
    // httplib — atomic, мьютекс ради одного enum не нужен.
    std::atomic<llm::ProbeResult> llm_status{llm::ProbeResult::Disabled};
};
