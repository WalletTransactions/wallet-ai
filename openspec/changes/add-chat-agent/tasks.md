# Tasks

## 1. Полосы очереди

- [ ] 1.1 Добавить `jobs::Lane` в `Job`, `Store::create(…, lane)`, `Store::take(lane)` и `AI_QUEUE_MAX` на полосу (D5). Импорт и `parse_expense` — `Local`. Проверка: новые случаи в `tests/test_jobs.cpp` — облачное задание берётся при занятом локальном воркере, `busy` считается по полосе, фоновое по-прежнему уступает интерактивному
- [ ] 1.2 Запуск `AI_CLOUD_MAX_CONCURRENT` (2) воркеров облачной полосы в `main.cpp`, переменная в README. Проверка: `ctest` зелёный, существующие тесты `parse_expense` и импорта не изменились

## 2. Провайдеры и настройки

- [ ] 2.1 Миграция `migrations/000002_ai_user_settings.{up,down}.sql` (`ai_user_settings`, `ai_provider_consent`, D7) и методы репозитория в `db.{hpp,cpp}`. Проверка: мигратор накатывает и откатывает миграцию на test-базе; тест репозитория на `mem_repo`
- [ ] 2.2 `providers::Registry` и доступность (`not_configured`, `not_allowed`, проба LLM), разбор `ANTHROPIC_ALLOWED_USERS` (D8). Проверка: юнит-тесты — `*`, список, пустой ключ, LM Studio с пробой не `ok`
- [ ] 2.3 `CredentialResolver` с `owner: instance` (D9). Проверка: юнит-тест — ключ не попадает в сериализацию настроек и в лог-поля
- [ ] 2.4 `GET/PUT /api/ai/settings` по спеке `ai-providers` (`provider_unavailable`, `consent_required`, `settings_unavailable`, отзыв согласия). Описать `AiSettings` в `openapi.yaml`, раздел «Настройки ИИ» в README. Проверка: юнит-тесты хендлера (`test_ai.cpp`); `curl` на test-контуре — выбор `claude` без согласия даёт 409, с согласием 200

## 3. Адаптеры моделей

- [ ] 3.1 Нейтральная модель сообщений и интерфейс `chat::Model` (D2). Проверка: компилируется, `FakeModel` для тестов цикла по сценарию
- [ ] 3.2 `OpenAiChatModel`: тело с `tools`, разбор `tool_calls`/текста/`finish_reason`, `role: tool`, `strip_think`, битые аргументы. Проверка: юнит-тесты разбора на записанных ответах LM Studio (tool call, текст, `length`)
- [ ] 3.3 `AnthropicChatModel`: тело (D2), разбор `tool_use`/`text`/`stop_reason` (`refusal`, `max_tokens`), append-only `content`, `usage`. До реализации сверить `anthropic-version`, beta-заголовок fallback и форму `output_config` с актуальной документацией Claude API. Проверка: юнит-тесты разбора на образцах ответов; ручной вызов `tools/wallet_ai_chat_try --provider claude` на test-ключе отвечает текстом

## 4. MCP-клиент и обмен токена

- [x] 4.1 `auth_exchange::Client` (`/internal/token/exchange`, D4) с разбором ответов. Проверка: юнит-тесты разбора `200`/`400`/`401 invalid_client`/сети — тесты: `auth_exchange_parse_ok`, `auth_exchange_parse_400_codes_invalid`, `auth_exchange_parse_401_invalid_client_is_config_error`, `auth_exchange_parse_5xx_and_broken_200_unavailable`, `auth_exchange_http_sends_rfc8693_form_and_secret`, `auth_exchange_http_invalid_and_invalid_client`, `auth_exchange_http_network_and_not_configured` (`tests/test_auth_exchange.cpp`)
- [x] 4.2 `mcp::Client`: `initialize` (с `instructions`), `tools/list`, `tools/call`, `isError`, `401` → `auth_expired`, `503`/сеть → `tools_unavailable`, обрезка результата (D3). Проверка: юнит-тесты на фейковом HTTP; вызов против живого wallet-mcp test-контура токеном из 4.1 возвращает 6 инструментов — тесты: `mcp_truncate_text_utf8`, `mcp_split_url`, `mcp_parse_rpc_response_statuses`, `mcp_failure_codes`, `mcp_parse_tool_call_result`, `mcp_parse_tools_list_skips_nameless`, `mcp_client_initialize_list_call`, `mcp_client_tool_is_error_and_rpc_error`, `mcp_client_truncates_long_result`, `mcp_client_401_auth_expired`, `mcp_client_503_and_closed_port_tools_unavailable` (`tests/test_mcp_client.cpp`, фейк wallet-mcp на 127.0.0.1). Живой вызов против test-контура НЕ выполнен — сделать вместе с 6.3

## 5. Задание chat

- [ ] 5.1 Проверка `input` (`ChatInput`), вид `chat` в `POST /api/ai/jobs`: включение по конфигурации, выбор провайдера и полосы, обмен токена в хендлере, 503 `provider_unavailable`. Описать `ChatInput`/`ChatResult` в `openapi.yaml`. Проверка: юнит-тесты хендлера — 400 на пустой текст, 503 без `WALLET_MCP_URL`, 503 при выбранном недоступном `lmstudio`, токен не виден в снимке задания
- [ ] 5.2 Агентный цикл (`src/chat.cpp`): пределы шагов, финальный вызов без инструментов, дедлайн, неизвестный инструмент, `failure`-коды по спеке. Проверка: юнит-тесты на `FakeModel` + фейковом MCP — «Няня» → ошибка → «Baby Sitter» → текст (`steps` 3); 5 шагов инструментов → шестой без инструментов; дедлайн → `timeout`; отказ ключа → `provider_auth_failed` и запись журнала с `owner`
- [ ] 5.3 История диалога (D6): пары «вопрос — ответ», 6 пар, TTL, изоляция по пользователю, генерация `conversation_id`. Проверка: юнит-тесты — уточнение периода видит прежние реплики; чужой `conversation_id` — пустая история; через TTL — пустая
- [ ] 5.4 Промпт `prompts/chat.txt` (D10) с версией, загрузка при старте. Проверка: юнит-тест подстановок; без файла сервис не стартует (как с `expense.txt`)
- [ ] 5.5 Метрики `ai_chat_tokens_total{provider,direction}`, `ai_chat_jobs_total{provider,result}`, `ai_chat_tool_calls_total{tool,result}`, `ai_chat_steps`, запись журнала завершения без текстов. Проверка: `GET /metrics` после прогона; в журнале нет текста вопроса и сумм

## 6. Конфиг, документация, сквозная проверка

- [ ] 6.1 Переменные (`WALLET_MCP_URL`, `AUTH_EXCHANGE_SECRET`, `ANTHROPIC_API_KEY`, `ANTHROPIC_MODEL`, `ANTHROPIC_MAX_TOKENS`, `ANTHROPIC_EFFORT`, `ANTHROPIC_FALLBACKS`, `ANTHROPIC_ALLOWED_USERS`, `AI_DEFAULT_PROVIDER`, `AI_CLOUD_MAX_CONCURRENT`, `AI_JOB_TIMEOUT_SECS_CHAT`, `AI_CHAT_MAX_STEPS`, `AI_CHAT_HISTORY_TURNS`, `AI_CHAT_HISTORY_TTL_SECS`, `AI_CHAT_TOOL_RESULT_MAX_CHARS`, `LLM_MAX_TOKENS_CHAT`, `LLM_REASONING_CHAT`) в `main.cpp`, `.env.example` и таблице README; раздел README «Чат» (цикл, провайдеры, приватность). Проверка: сервис стартует с пустыми новыми переменными, `parse_expense` работает
- [ ] 6.2 Сборка и тесты в Docker (`Containerfile`, стадия `test`) — по одному сервису. Проверка: `ctest` зелёный в контейнере
- [ ] 6.3 Живой прогон на test-контуре с LM Studio: задание `chat` «сколько я потратил на няню и еду в сентябре?» токеном бота. Проверка: ответ содержит итог, совпадающий с `/api/reports/expense-structure` за сентябрь, `result.tools` содержит `spending_by_category`, время — в пределах оценки D11
- [ ] 6.4 Тот же прогон с `provider: claude` (тестовый пользователь в `ANTHROPIC_ALLOWED_USERS`, согласие дано). Проверка: ответ совпадает по сумме, задание выполнено облачной полосой при занятом локальном воркере, `ai_chat_tokens_total{provider="claude"}` вырос

## Workflow follow-up

- wallet-infra: переменные wallet-ai для test и prod, сеть wallet-ai ↔ wallet-mcp, секрет `wallet-ai` в `TOKEN_EXCHANGE_SECRETS` google-auth.
- Обновить `org/docs/TODO_wallet_ai.md` (пункт #8, раздел про `chat`) по итогам.
- Архивировать после выката wallet-telegram `add-ai-chat` и сквозных проверок.
