// Состояние процесса wallet-ai. Задания живут в памяти (jobs.hpp); импорты
// выписок — в базе ai (statement/repo.hpp), она необязательна.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "auth_exchange.hpp"
#include "chat.hpp"
#include "clients/reports_client.hpp"
#include "db.hpp"
#include "imports.hpp"
#include "jobs.hpp"
#include "llm_client.hpp"
#include "llm_probe.hpp"
#include "mcp_client.hpp"
#include "parse_expense.hpp"
#include "providers.hpp"
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

    // Обработчики фоновых заданий импорта по kind (extract_rows,
    // classify_rows) — регистрирует ImportRunner; воркер выбирает по kind.
    std::map<std::string, std::function<parse_expense::Outcome(const jobs::Job&)>> job_handlers;

    // База ai отвечает и схема накачена (фоновая проба в main.cpp). Без неё
    // импорт выписок выключен, parse_expense работает.
    std::atomic<bool> db_up{false};
    // DATABASE_URL задан и промпты импорта загружены.
    bool imports_enabled = false;

    // Импорт выписок (add-statement-import): пул базы ai, хранилище, логика
    // ручек и планировщик. nullptr — импорт выключен.
    std::unique_ptr<db::Pool> db;
    std::unique_ptr<stmt::Repo> repo;
    std::unique_ptr<imports::Service> import_service;
    std::unique_ptr<imports::Runner> import_runner;

    // --- Чат (add-chat-agent) ---

    // Провайдеры (lmstudio, claude) и их адаптеры; ключ — на каждый вызов.
    std::unique_ptr<providers::Registry> providers;
    std::unique_ptr<providers::CredentialResolver> credentials;
    // Настройки ИИ пользователя в базе ai; выставляется до db_up = true.
    // nullptr или база не отвечает — провайдер по умолчанию, PUT — 503.
    std::unique_ptr<providers::SettingsRepo> settings_repo;
    // Заданы WALLET_MCP_URL и AUTH_EXCHANGE_SECRET; иначе chat — 503
    // unavailable (Migration Plan п. 2).
    bool chat_enabled = false;
    std::unique_ptr<auth_exchange::Client> auth_exchange;
    std::unique_ptr<mcp::Client> mcp;
    std::unique_ptr<chat::Conversations> conversations;
    std::unique_ptr<chat::Agent> chat_agent;
};
