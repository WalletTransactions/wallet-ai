// Публичное API /api/ai/* (org/docs/TODO_wallet_ai.md, §3). Шаг 1 — только
// GET /api/ai/status: по нему веб показывает или прячет пункт «ИИ», бот —
// строку про свободный текст в /help.
#pragma once

#include "app_state.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"

namespace aiapi {

// Тело GET /api/ai/status — чистая функция для тестов.
// enabled — LLM настроена и отвечает с нужной моделью; features — что уже
// умеет сервис (пусто до шага 3: parse_expense).
nlohmann::json status_json(const llm::Config& cfg, llm::ProbeResult status);

void register_routes(httplib::Server& svr, AppState& state);

} // namespace aiapi
