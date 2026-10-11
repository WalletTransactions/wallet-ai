# wallet-ai

Единая точка доступа к LLM для всех клиентов WalletTransactions: разбор трат
свободным текстом, импорт банковских выписок, чат с вопросами о тратах.
План — [`org/docs/TODO_wallet_ai.md`](../org/docs/TODO_wallet_ai.md).

```
браузер ─> wallet-gateway ─ /api/ai/* ─> wallet-ai ─> LLM (LM Studio на хосте, OpenAI API)
Telegram ─> wallet-telegram ─────────────> wallet-ai
```

**Сервис ничего не пишет в кошелёк.** Он возвращает черновики и читает
данные токеном пользователя; запись делает клиент после подтверждения:
бот — по кнопке, веб — `POST /sync/push` в wallet-api.

## Состояние

Шаги 1–3 из §10 плана: каркас, проверка токена, `GET /api/ai/status`,
фоновая проба LLM, `llm_client`, асинхронные задания и `parse_expense` —
трата свободным текстом в проверенный черновик. Бот и веб к заданиям ещё не
подключены (шаги 4–5).

## API

| Метод | Путь | Что |
| --- | --- | --- |
| GET | `/api/ai/status` | `{enabled, model, reason, features}` — включён ли ИИ |
| POST | `/api/ai/jobs` | `{kind, input, idempotency_key?}` → `202 {job_id}`; ИИ выключен — `503` |
| GET | `/api/ai/jobs/{id}?wait=N` | статус и результат; `wait` ≤ 25 с — long-poll |
| DELETE | `/api/ai/jobs/{id}` | отменить |
| GET | `/healthz`, `/metrics`, `/version`, `/docs`, `/openapi.yaml` | служебное, без токена |

### Задания

Всё, что зовёт LLM, — асинхронно (`org/docs/TODO_wallet_ai.md`, §3–§5):
модель думает столько, сколько нужно виду задания, а клиент не упирается
ни в таймаут гейтвея (30 с), ни в свой поток.

```bash
curl -X POST $GW/api/ai/jobs -H "Authorization: Bearer $T" -d '{"kind":"parse_expense",
  "input":{"text":"вчера такси 450 с тинькоффа","now":{"date":"2026-10-11","time":"14:05:00"}},
  "idempotency_key":"tg:123:456"}'                       # -> 202 {"job_id":"…","status":"queued"}
curl "$GW/api/ai/jobs/$ID?wait=25" -H "Authorization: Bearer $T"
# -> {"status":"done","result":{"outcome":"draft","expense":{…},"resolution":{…}}}
```

- **Очередь.** `LLM_MAX_CONCURRENT` воркеров (одна видеокарта — один);
  интерактивные задания раньше фоновых.
- **Хранение.** Задания живут в памяти: вход — до завершения, результат —
  `AI_JOB_TTL_SECS` после. Рестарт сервиса их теряет, и это допустимо.
- **Доступ.** Задание видит только владелец, на чужой id — 404.
- **`parse_expense`.** Справочник счетов берётся токеном пользователя при
  создании задания. Затем `llm_client`, проверки (`src/expense_check.cpp`:
  сумма есть в тексте, дата не в будущем и не старше года, валюта известная)
  и `resolver` из `wallet-common/expense`. Исходы:
  - `draft` — черновик и решение по счетам;
  - `not_expense` — модель не нашла трату;
  - `rejected` — проверки не прошли, причины в `reasons`.

  Если LLM недоступна, ответила по таймауту или битым JSON, задание
  получает `failed`.

Полный контракт — [`openapi.yaml`](openapi.yaml), Swagger UI — `GET /docs`.

## Авторизация

Access-токен google-auth (Bearer или cookie `session`), подпись по JWKS.
В `aud` должен быть `wallet-ai`, в `scope` — `ai:use`
(`google-auth/src/token_claims.hpp`: есть у токенов пользователя и бота).
Токен без `aud` (старого формата) отвергается 401: `ai:use` в нём нет.

## Конфиг

| Переменная | По умолчанию | Смысл |
| --- | --- | --- |
| `AUTH_SERVICE_URL` | — | google-auth, JWKS |
| `LLM_URL` | пусто | база OpenAI API (`http://host.docker.internal:1234/v1`). Пусто — ИИ выключен, `status.enabled=false` |
| `LLM_MODEL` | — (обязательна при `LLM_URL`) | id из `GET {LLM_URL}/models` |
| `LLM_API_KEY` | пусто | `Authorization: Bearer` к LLM |
| `LLM_TIMEOUT_MS` | `15000` | read-таймаут; connect — 2 с |
| `LLM_PROBE_INTERVAL_SECS` | `60` | как часто проверять LLM |
| `LLM_PROMPT_DIR` | `prompts` | где лежат промпты (`expense.txt`); нет файла — сервис не стартует |
| `WALLET_REPORT_URL` | — | wallet-report: справочник счетов токеном пользователя |
| `LLM_MAX_CONCURRENT` | `1` | воркеров заданий = одновременных запросов к GPU |
| `LLM_REASONING_PARSE_EXPENSE` | `none` | `reasoning_effort` для `parse_expense`; `default` — не передавать |
| `LLM_MAX_TOKENS_PARSE_EXPENSE` | `512` | запас под рассуждения модели |
| `AI_JOB_TIMEOUT_SECS_PARSE_EXPENSE` | `60` | дедлайн генерации задания |
| `AI_JOB_TTL_SECS` | `600` | сколько хранить завершённое задание |
| `AI_QUEUE_MAX` | `20` | интерактивных заданий в очереди; больше — `429 busy` |
| `AI_MAX_ACTIVE_PER_USER` | `3` | незавершённых заданий на пользователя |
| `LLM_MAX_PER_MINUTE` | `10` | новых заданий на пользователя в минуту |
| `PORT` / `HOST` | `8087` / `127.0.0.1` | |
| `LOG_DIR` / `LOG_LEVEL` | stdout / debug | |

LLM необязательна: недоступна — сервис стартует и отвечает
`enabled=false`, клиенты работают без ИИ.

## LLM на хосте (LM Studio)

GPU и модель — на хосте, вне docker. Проверено 2026-10-11 на GTX 1070 8 ГБ
с `google/gemma-4-e4b` (детали — «Шаг 9 подробно» в
`org/docs/TODO_telegram_bot.md`).

Пункты 1–2 и проверку из п. 4 делает `bash wallet-ai/scripts/lms-load.sh`:
запускает сервер на `0.0.0.0:1234`, если он не запущен, грузит модель под id
`wallet-parse`, если её нет, и проверяет `GET /v1/models`. Повторный запуск
ничего не меняет. Другая модель или id — через переменные (`LMS_MODEL_KEY`,
`LMS_IDENTIFIER`, ... — см. шапку скрипта); при включённом токене LM Studio —
`LLM_API_KEY=... bash wallet-ai/scripts/lms-load.sh`.

Вручную:

1. Загрузить модель под стабильным id, без JIT:
   `lms load google/gemma-4-e4b --identifier wallet-parse --context-length 4096 --gpu max`.
   Авто-выгрузку по простою выключить.
2. Сервер: `lms server start --port 1234`. В GUI (Developer → Server
   Settings) — «Serve on Local Network», иначе из контейнера не достучаться;
   порт в файрволе открыть только для docker-сетей (`172.16.0.0/12`).
3. Включить токен API, тот же — в `LLM_API_KEY`. Логи запросов (verbose)
   выключить: там окажутся тексты трат.
4. Проверка: `curl -H "Authorization: Bearer …" http://127.0.0.1:1234/v1/models`
   — в списке `wallet-parse`.

## Разбор траты: `llm_client`

`src/llm_client.{hpp,cpp}` — один вызов `POST {LLM_URL}/chat/completions`:
`response_format: json_schema` (strict), категории и кошельки — `enum` из
имён пользователя плюс `""` (llama.cpp превращает схему в грамматику — чужое
имя модель вернуть не может), в system — сегодняшняя дата и календарь 7
прошедших дней, три примера из имён пользователя, текст пользователя — только
в последнем user-сообщении. Дедлайн — на весь запрос (`set_max_timeout`).
Зовётся воркером задания `parse_expense`; проверки полей — там же.

**Промпт — файл [`prompts/expense.txt`](prompts/expense.txt)**, не код:
`# version:` в шапке (пишется в логи и eval), подстановки `{today}`,
`{recent_days}`, `{categories}`, … Читается при старте — правка без
пересборки, нужен рестарт. Примеры (few-shot) строятся в коде из имён
пользователя: их ответы обязаны проходить ту же JSON-схему.

Ручная проверка против живой LLM — `wallet_ai_llm_try`
(`-DWALLET_AI_BUILD_TOOLS=ON`, в образ не входит):

```bash
LLM_URL=http://127.0.0.1:1234/v1 LLM_MODEL=wallet-parse ./build/wallet_ai_llm_try \
  --categories "Кафе,Продукты,Такси" --wallets "Тинькофф:RUB,Revolut:EUR" \
  --today 2026-10-11 [--reasoning none] [--max-tokens 512] "вчера такси 450" "12 евро обед"
# --dump-body — напечатать тело запроса и выйти
```

Замечено на gemma-4-e4b (LM Studio, GTX 1070, 2026-10-11, промпт `expense-v2`):
`reasoning_effort: "none"` рассуждения не выключает, а сокращает — 0–370
токенов, 2–13 с на фразу, поэтому `max_tokens` 512; другие способы
выключить (`low`, `minimal`, `chat_template_kwargs`) LM Studio игнорирует.
Поля на пробных фразах верные, включая «в пятницу», «24.09», «12 евро» →
Revolut. Выбор модели — eval (шаг 10 плана бота).

## Сборка

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

В docker — из `wallet-infra/`:
`docker compose --env-file infra/db/.env up -d --build wallet-ai`.
Юнит-тесты в образе: `docker build --target test -f Containerfile .`.

## Общий код

`third_party/wallet-common/` — копия репозитория `wallet-common`: `util/`
(JWT, JWKS, логи, метрики, ...), `expense/` (разбор суммы и дат, resolver —
тот же, что у строгого формата бота), `clients/` (клиент wallet-report). Руками не править: правка — в `wallet-common`, затем
`bash ../wallet-common/scripts/vendor.sh`.

## Метрики

- `llm_up` — 1, если последняя проба нашла модель;
- `ai_job_create_total{result}` — `created`, `reused`, `busy`, `rate_limited`,
  `too_many_active`, `unavailable`;
- `ai_job_result_total{result}` — `draft`, `not_expense`, `rejected`, `failed_<причина>`;
- `ai_job_duration_seconds{kind}`, `ai_queue_wait_seconds{kind}`,
  `ai_queue_interactive`, `ai_queue_background`;
- `ai_jobs_by_platform_total{platform}` — откуда задания (web, telegram, …);
- `ai_reports_errors_total{kind}` — ошибки запросов к wallet-report;
- `jwt_claims_total{result}` — проверки iss/aud/scope;
- `http_requests_total`, `http_request_duration_seconds` — как у всех сервисов.
