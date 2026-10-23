// Публичное API /api/ai/* (org/docs/TODO_wallet_ai.md, §3):
//   GET    /api/ai/status            — включён ли ИИ и что умеет
//   POST   /api/ai/jobs              — создать задание -> 202 {job_id}
//   GET    /api/ai/jobs/{id}?wait=N  — статус и результат, long-poll ≤ 25 с
//   DELETE /api/ai/jobs/{id}         — отменить
// и воркер, который выполняет задания (main.cpp запускает LLM_MAX_CONCURRENT).
#pragma once

#include "app_state.hpp"
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "parse_expense.hpp"

namespace aiapi {

// Дольше держать GET нельзя: у гейтвея read-таймаут к апстриму 30 с.
inline constexpr int kMaxWaitSecs = 25;

// Тело GET /api/ai/status — чистая функция для тестов.
// enabled — LLM настроена и отвечает с нужной моделью; features — виды
// заданий, которые сейчас можно создать.
// statement_import — импорт выписок настроен и база ai отвечает; в features
// он попадает только вместе с включённым ИИ.
nlohmann::json status_json(const llm::Config& cfg, llm::ProbeResult status, bool statement_import = false);

// Снимок задания для GET — чистая функция.
nlohmann::json job_json(const jobs::Job& job);

void register_routes(httplib::Server& svr, AppState& state);

// Обработчик задания по kind: parse_expense, виды из state.job_handlers
// (импорт выписки), неизвестный — failed: internal без вызова LLM.
parse_expense::Outcome run_job(AppState& state, const jobs::Job& job);

// Цикл воркера: берёт задания, пока store не остановлен.
void run_worker(AppState& state);

} // namespace aiapi
