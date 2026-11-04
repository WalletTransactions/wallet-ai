# Design

## Context

- `llm::Client` (`src/llm_client.{hpp,cpp}`) делает одиночные вызовы
  `POST {LLM_URL}/chat/completions` со своим промптом на вид задания. Поддержки
  `tools`/`tool_calls` нет.
- `jobs::Store` — одна пара очередей (интерактивная/фоновая), воркеров
  `LLM_MAX_CONCURRENT`. `ai.cpp` знает только `parse_expense`, а при
  создании проверяет пробу LLM и сразу ходит в wallet-report токеном
  пользователя.
- База ai (`DATABASE_URL`) необязательна: без неё выключен импорт.
- wallet-mcp отдаёт 6 read-инструментов и `instructions` в `initialize`
  (дата, правила периодов, «переспроси при неоднозначном имени»).
- Клиенты wallet-ai: бот (токен `aud` ∋ `wallet-ai`, `scope`
  `reports:read expense:write ai:use`, 300 с, берётся на команду) и веб.
- Мотивация — proposal.md. Требования — `specs/ai-chat`, `specs/ai-providers`,
  `specs/ai-jobs`.

## Goals / Non-Goals

**Goals:**
- Один агентный цикл, независимый от провайдера; провайдер — адаптер.
- Инструменты исполняет только wallet-ai через wallet-mcp; провайдер не
  получает ни токенов, ни сетевого доступа к стеку.
- Швы под собственный ключ пользователя без переделки цикла и адаптеров.

**Non-Goals:**
- Стриминг ответа (бот всё равно правит одно сообщение).
- MCP connector Anthropic и `ephemeral_mcp` LM Studio (провайдер сам ходит в
  MCP). Это потребовало бы публичного HTTPS у wallet-mcp, см. D1.
- Инструменты записи и `draft_expense`.
- Хранение истории в Redis или БД.

## Decisions

### D1. Свой цикл, а не «провайдер сам вызывает MCP»
Цикл: модель → `tool_calls` → `tools/call` wallet-mcp → результаты модели →
… до текста. Альтернативы:
- MCP connector Anthropic — Anthropic подключается к wallet-mcp из своего
  облака, нужен публичный HTTPS;
- `/api/v1/chat` + `ephemeral_mcp` LM Studio — нативный API LM Studio,
  токен проходит через её процесс.

Свой цикл даёт одинаковые для всех провайдеров пределы, метрики и
приватность, а своей модели позже достаточно уметь tool calling.

### D2. Нейтральная модель сообщений и два адаптера
`ChatMessage { role: user|assistant|tool; text; tool_calls[{id,name,args}];
tool_results[{id, content, is_error}]; raw }`. `raw` — исходный JSON ответа
провайдера, адаптер отдаёт его обратно без изменений.
Интерфейс `chat::Model::turn(system, messages, tools, credential, deadline)
→ Turn { kind: text|tool_calls|refused|truncated; message; usage }`.

- **`OpenAiChatModel`** (lmstudio, своя модель). `POST {LLM_URL}/chat/completions`:
  - `tools` в форме `{type:"function", function:{name, description, parameters}}`;
  - ответ — `choices[0].message.tool_calls`, результаты возвращаются как
    `role: "tool"`, `tool_call_id`;
  - `reasoning_effort` и `max_tokens` из `LLM_REASONING_CHAT` (`none`) и
    `LLM_MAX_TOKENS_CHAT` (1024);
  - вырезка `<think>` (`strip_think`) переиспользуется;
  - битые аргументы (не JSON) возвращаются модели ошибкой инструмента.
- **`AnthropicChatModel`** (claude). Сырой HTTPS `POST
  https://api.anthropic.com/v1/messages`: у C++ нет официального SDK.
  - Заголовки: `x-api-key`, `anthropic-version`, `content-type`.
  - Тело:
    - `model` из `ANTHROPIC_MODEL`, по умолчанию `claude-opus-5-5`;
    - `max_tokens` из `ANTHROPIC_MAX_TOKENS` (16000);
    - `system`;
    - `tools` в форме `{name, description, input_schema}`, `tool_choice`
      `auto` (принудительный выбор на Opus 5.5 даёт 400);
    - `thinking: {type: "adaptive"}`;
    - `output_config: {effort}` из `ANTHROPIC_EFFORT` (`low`: для чата
      этого достаточно, расход меньше);
    - `temperature`/`top_p` не передаются.
  - Ответ ассистента (`content` целиком, вместе с блоками `thinking`)
    добавляется в историю цикла **без изменений**. Все `tool_result`
    одного хода уходят одним сообщением `user`. Так история остаётся
    append-only, и проверка preserved thinking не срабатывает.
  - Между заданиями хранится только текст (D6), блоки `thinking` в новые
    задания не переносятся.
  - `stop_reason`: `tool_use` → вызовы; `end_turn` → текст;
    `refusal` → `provider_refused`; `max_tokens` → `truncated`
    (`failed: internal`).
  - Серверный fallback при отказе (`fallbacks: "default"`, beta
    `server-side-fallback-2026-07-01`) включён по умолчанию, выключается
    `ANTHROPIC_FALLBACKS=off`.
  - Точное значение `anthropic-version` и beta-заголовков сверить с
    актуальной документацией при реализации (tasks 3.3).

Разбор ответов обоих адаптеров — чистые функции, как
`parse_expense_response`.

### D3. MCP-клиент
`mcp::Client` по `WALLET_MCP_URL` (`http://wallet-mcp:8088/api/mcp/`,
напрямую, не через гейтвей). На каждое задание — `initialize` (берётся
`instructions`) и `tools/list`, затем `tools/call`. Заголовок
`MCP-Protocol-Version` — `2025-11-25`. wallet-mcp не хранит сессий, поэтому
лишние два запроса по внутренней сети стоят миллисекунды, а схемы всегда
актуальны (без кеша и его инвалидации).
- Ответ `isError` уходит модели как результат с `is_error`.
- `401` → `auth_expired`.
- Сеть или `503` → `tools_unavailable`.

Результат инструмента длиннее `AI_CHAT_TOOL_RESULT_MAX_CHARS` (6000) обрезается
с пометкой «…(обрезано)»: окно LM Studio 8192.

### D4. Токен для wallet-mcp — обменом при создании задания
`auth_exchange::Client` → `POST {AUTH_SERVICE_URL}/internal/token/exchange`
(`X-Service-Secret` = `AUTH_EXCHANGE_SECRET`, `audience=wallet-mcp`). Обмен
идёт в хендлере создания, как сейчас запрос справочника для
`parse_expense`: токен пользователя свежий, а к запуску задания мог бы
истечь. Выданный токен лежит во `input` задания (в памяти, стирается после
завершения), в снимок и журнал не попадает. Срок — не больше остатка
токена пользователя (у бота 300 с от команды): этого хватает на очередь и
дедлайн 120 с.

### D5. Полосы очереди
`jobs::Lane { Local, Cloud }` во `Job`. Полоса определяется при создании
провайдером задания: `parse_expense` и импорт — всегда `Local`.
`Store::take(lane)`. Воркеры: `LLM_MAX_CONCURRENT` на `Local`,
`AI_CLOUD_MAX_CONCURRENT` на `Cloud`. Интерактивная и фоновая очереди есть
только у `Local`, `Cloud` — одна интерактивная. `AI_QUEUE_MAX` — на полосу.
Лимиты пользователя (`AI_MAX_ACTIVE_PER_USER`, `LLM_MAX_PER_MINUTE`) общие:
они защищают от злоупотребления, а не GPU.

### D6. История — в памяти, только тексты
`chat::Conversations` — map `(user_id, conversation_id) → {turns, last_used}`
под мьютексом, чистится тем же фоновым потоком, что задания.
- В историю пишутся пары «вопрос — итоговый ответ», не больше 6.
- Результаты инструментов не сохраняются: они раздули бы окно 8k, а модель
  при уточнении всё равно вызовет инструмент заново.
- Рестарт теряет историю. Это допустимо по той же логике, что потеря
  заданий.
- Без `conversation_id` сервис создаёт UUID и возвращает его.

### D7. Провайдеры, настройки, согласие
Реестр `providers::Registry` хранит для каждого провайдера `id`, `title`,
`cloud`, доступность для пользователя (D8) и фабрику адаптера. Миграция
`000002_ai_user_settings`:
- `ai_user_settings(user_id uuid PK, provider text, updated_at)`;
- `ai_provider_consent(user_id, provider, consented_at, PK(user_id, provider))`.

Согласие — отдельной таблицей на провайдера: при второй облачной модели
схема не меняется. Без базы `GET` отдаёт провайдера по умолчанию, `PUT` — 503.

### D8. Доступ к Claude — список в конфиге, а не scope в JWT
При обсуждении предполагался scope `ai:claude`. Решено иначе: список
`ANTHROPIC_ALLOWED_USERS` (`user_id` через запятую или `*`). Scope
потребовал бы правок google-auth в двух наборах claims (веб и бот) и
админского интерфейса выдачи. Список в `.env` wallet-ai решает задачу
«кому стек оплачивает Claude» без правок других сервисов.

### D9. Ключ — на каждый вызов (шов под вариант b)
`providers::CredentialResolver::resolve(user_id, provider) → optional<Credential
{secret, owner: instance|user}>`. Сейчас возвращает `ANTHROPIC_API_KEY` с
`owner: instance` или `nullopt`. Адаптер получает `Credential` в каждом
`turn`, глобального клиента с ключом нет. Вариант b добавит таблицу
`ai_user_keys` и ветку `owner: user` в resolver. Цикл, адаптеры и задания
не меняются. `owner` идёт в журнал отказа ключа (`provider_auth_failed`).

### D10. Промпт
`prompts/chat.txt` (`# version: chat-v1`) с подстановками `{today}`,
`{weekday}`, `{time}` (из `input.now`, иначе UTC сервера) и
`{mcp_instructions}`. Правила:
- отвечать по-русски, обычным текстом без markdown;
- суммы брать только из инструментов и называть период и валюту;
- при неоднозначном имени или неясном периоде переспросить.

Текст пользователя идёт только в последнее сообщение `user`.

### D11. Нагрузка на GPU (lmstudio)
| Часть | Токенов |
| --- | --- |
| system + `instructions` | ~500 |
| схемы 6 инструментов | ~1500 |
| история до 6 пар | ~600 |
| результаты инструментов | ~200–1500 |

Итого 2,5–4k токенов на шаг. На GTX 1070 (gemma-4-e4b) один шаг — около
3–5 с обработки промпта и 2–13 с генерации. Обычный вопрос — 2–3 шага, то
есть 15–45 с. Худший случай — 6 вызовов, около 90 с, поэтому дедлайн 120 с.
`LLM_CONTEXT` должен быть не меньше 8192: при 4096 схемы и история не
поместятся.

### Правки в других сервисах
- **google-auth:** `add-token-exchange` и секрет `wallet-ai` в `TOKEN_EXCHANGE_SECRETS`.
- **wallet-mcp:** `add-jwt-access` (приём JWT `aud` `wallet-mcp`).
- **wallet-telegram:** клиент чата и `/ai` (`add-ai-chat`).
- **wallet-infra:**
  - переменные wallet-ai: `WALLET_MCP_URL`, `AUTH_EXCHANGE_SECRET`,
    `ANTHROPIC_*`, `AI_CLOUD_MAX_CONCURRENT`, `AI_DEFAULT_PROVIDER`;
  - wallet-ai и wallet-mcp в одной docker-сети;
  - исходящий HTTPS из wallet-ai.
- **wallet-gateway:** без правок (`/api/ai/*`).
- **wallet-common:** без правок.

## Risks / Trade-offs

- [Маленькая модель путается в tool calling] → живой прогон gemma-4-e4b с
  wallet-mcp прошёл. Битые вызовы возвращаются модели ошибкой и тратят
  шаг. Если упрёмся — eval (пункт плана) и проверка Qwen3.
- [Токен истекает во время задания (очередь + 120 с > остатка токена)] →
  `auth_expired`, бот показывает запасной ответ. Для бота маловероятно:
  токен берётся на команду.
- [Стоимость Claude Opus 5.5] → `ANTHROPIC_MODEL` настраивается (например,
  `claude-haiku-5-5`), effort `low`, доступ по списку, расход в метриках.
- [Данные уходят в Anthropic] → только после явного согласия, отзыв
  согласия сразу возвращает `lmstudio`.
- [Prompt injection через комментарии операций] → инструменты только на
  чтение и только данные самого пользователя. Ответ видит только он.
- [Размер ответа `search_operations`] → обрезка результата (D3), у
  инструмента есть `limit`.

## Migration Plan

1. Порядок выката: google-auth `add-token-exchange` → wallet-mcp
   `add-jwt-access` → wallet-ai (миграция `000002` мигратором) →
   wallet-telegram `add-ai-chat`.
2. Без `WALLET_MCP_URL` или `AUTH_EXCHANGE_SECRET` вид `chat` выключен:
   создание отвечает 503 `{"error":"unavailable"}`, `parse_expense` и
   импорт работают как раньше.
3. Claude включается заданием `ANTHROPIC_API_KEY` и `ANTHROPIC_ALLOWED_USERS`.
4. Откат: снять `WALLET_MCP_URL`. Миграция `000002` безопасна для старой
   версии: новые таблицы она не читает.
