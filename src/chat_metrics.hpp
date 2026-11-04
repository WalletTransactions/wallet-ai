// Метрики задания chat (change add-chat-agent, tasks 5.5). util/metrics
// (wallet-common, вендорен) умеет только одну метку и +1, а здесь нужны
// счётчики с двумя метками и прибавкой токенов, поэтому — свой маленький
// реестр; GET /metrics отдаёт его вслед за metrics::render() (docs.cpp).
//
//   ai_chat_tokens_total{provider,direction}  — токены вызовов модели (input/output);
//   ai_chat_jobs_total{provider,result}       — завершённые задания chat по исходу;
//   ai_chat_tool_calls_total{tool,result}     — вызовы инструментов (ok/error/...);
//   ai_chat_steps                             — гистограмма вызовов модели на задание.
//
// Значения меток — только имена провайдеров, инструментов из tools/list и
// коды исхода: ни текстов, ни сумм.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace chat_metrics {

using Labels = std::vector<std::pair<std::string, std::string>>;

void add(const std::string& name, const std::string& help, const Labels& labels, std::uint64_t value = 1);
void observe_steps(int steps);

// Текущее значение счётчика (для тестов); нет такого — 0.
std::uint64_t value(const std::string& name, const Labels& labels);

// Prometheus text format.
std::string render();

} // namespace chat_metrics
