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

Шаг 1 из §10 плана: каркас, проверка токена, `GET /api/ai/status`, фоновая
проба LLM. Разбора трат ещё нет (шаги 2–3).

## API

| Метод | Путь | Что |
| --- | --- | --- |
| GET | `/api/ai/status` | `{enabled, model, reason, features}` — включён ли ИИ |
| GET | `/healthz`, `/metrics`, `/version`, `/docs`, `/openapi.yaml` | служебное, без токена |

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
Зовётся воркером задания `parse_expense` (шаг 3); проверки полей — там же.

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

`third_party/wallet-common/` — копия репозитория `wallet-common` (JWT, JWKS,
логи, метрики, ...). Руками не править: правка — в `wallet-common`, затем
`bash ../wallet-common/scripts/vendor.sh`.

## Метрики

- `llm_up` — 1, если последняя проба нашла модель;
- `jwt_claims_total{result}` — проверки iss/aud/scope;
- `http_requests_total`, `http_request_duration_seconds` — как у всех сервисов.
