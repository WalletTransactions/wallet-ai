# wallet-ai

Единая точка доступа к LLM для всех клиентов WalletTransactions: разбор трат
свободным текстом, импорт банковских выписок, чат с вопросами о тратах.
План — [`org/docs/TODO_wallet_ai.md`](../org/docs/TODO_wallet_ai.md).

```
браузер ─> wallet-gateway ─ /api/ai/* ─> wallet-ai ─> LLM (LM Studio на хосте, OpenAI API)
Telegram ─> wallet-telegram ─────────────> wallet-ai ─> Claude (api.anthropic.com, по выбору пользователя)
                                              └──────> wallet-mcp (инструменты чтения, чат)
```

**Сервис ничего не пишет в кошелёк.** Он возвращает черновики и читает
данные токеном пользователя; запись делает клиент после подтверждения:
бот — по кнопке, веб — `POST /sync/push` в wallet-api.

## Состояние

Шаги 1–3 из §10 плана: каркас, проверка токена, `GET /api/ai/status`,
фоновая проба LLM, `llm_client`, асинхронные задания и `parse_expense` —
трата свободным текстом в проверенный черновик. Задание `chat` — вопрос о
кошельке обычными словами (`openspec/changes/add-chat-agent`, см. «Чат»).

## API

| Метод | Путь | Что |
| --- | --- | --- |
| GET | `/api/ai/status` | `{enabled, model, reason, features}` — включён ли ИИ |
| POST | `/api/ai/jobs` | `{kind, input, idempotency_key?}` → `202 {job_id}`; ИИ выключен — `503` |
| GET / PUT | `/api/ai/settings` | провайдер чата (`lmstudio`, `claude`) и согласие на облако |
| GET | `/api/ai/jobs/{id}?wait=N` | статус и результат; `wait` ≤ 25 с — long-poll |
| DELETE | `/api/ai/jobs/{id}` | отменить |
| POST | `/api/ai/imports` | выписка (текст или файл) и кошелёк → `202 {import_id}` |
| GET | `/api/ai/imports`, `/api/ai/imports/{id}` | список; статус, прогресс, строки и группы |
| DELETE | `/api/ai/imports/{id}` | отменить разбор и удалить |
| POST | `/api/ai/imports/{id}/retry` | продолжить `failed` |
| PATCH / POST | `/api/ai/imports/{id}/rows` | правки строк; строка из «не разобрано» |
| POST | `/api/ai/imports/{id}/committed` | отметить записанные строки |
| GET / PATCH / DELETE | `/api/ai/statement-profiles[/{id}]` | профили формата выписки |
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

- **Очередь.** Две полосы. Локальная (LM Studio: `parse_expense`, импорт,
  чат через `lmstudio`) — `LLM_MAX_CONCURRENT` воркеров (одна видеокарта —
  один), интерактивные задания раньше фоновых. Облачная (чат через `claude`)
  — `AI_CLOUD_MAX_CONCURRENT` воркеров, GPU не ждёт. `AI_QUEUE_MAX` — на
  полосу; лимиты пользователя общие.
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

### Чат

Спецификация — `openspec/changes/add-chat-agent`. Клиент — бот
(`add-ai-chat` в wallet-telegram), позже веб.

```bash
curl -X POST $GW/api/ai/jobs -H "Authorization: Bearer $T" -d '{"kind":"chat",
  "input":{"text":"сколько я потратил на няню в сентябре?","conversation_id":"tg-42-1",
           "now":{"date":"2026-11-04","time":"14:05:00"}}}'   # -> 202 {"job_id":"…"}
curl "$GW/api/ai/jobs/$ID?wait=25" -H "Authorization: Bearer $T"
# -> {"status":"done","result":{"text":"…","conversation_id":"tg-42-1","provider":"lmstudio",
#     "steps":2,"tools":["spending_by_category"]}}
```

- **Включение.** Вид `chat` работает только при заданных `WALLET_MCP_URL`
  и `AUTH_EXCHANGE_SECRET`; без них создание отвечает `503 unavailable`, а
  `parse_expense` и импорт работают как раньше.
- **Токен для wallet-mcp.** При создании задания JWT пользователя
  обменивается в google-auth (`/internal/token/exchange`, actor `wallet-ai`,
  `audience=wallet-mcp`, `src/auth_exchange.cpp`). Отказ — `401
  invalid_token`, google-auth недоступен — `503 unavailable`. Выданный токен
  живёт только во входе задания в памяти: ни в снимок задания, ни в журнал
  он не попадает.
- **Цикл** (`src/chat.cpp`). На задание — `initialize` (берутся
  `instructions`) и `tools/list` wallet-mcp (`src/mcp_client.cpp`), затем
  модель → `tool_calls` → `tools/call` → результаты модели → … → текст. Не
  больше `AI_CHAT_MAX_STEPS` (5) вызовов модели; если модель всё ещё просит
  инструменты, шестой вызов идёт без них с просьбой ответить по полученным
  данным. Всё задание — `AI_JOB_TIMEOUT_SECS_CHAT` (120 с), потом `failed:
  timeout`. Инструмента нет в `tools/list` или аргументы не JSON — модели
  возвращается ошибка инструмента, в wallet-mcp ничего не уходит.
  Результат инструмента длиннее `AI_CHAT_TOOL_RESULT_MAX_CHARS` обрезается.
- **Отказы:** `tools_unavailable` (wallet-mcp лежит или 503), `auth_expired`
  (wallet-mcp ответил 401), `provider_auth_failed` (провайдер отклонил ключ,
  в журнале — `provider` и `owner`), `provider_refused` (модель отказалась),
  `unavailable` (провайдер недоступен), `timeout`, `internal`.
- **История.** В памяти по паре (пользователь, `conversation_id`): пары
  «вопрос — итоговый ответ», не больше `AI_CHAT_HISTORY_TURNS`, живут
  `AI_CHAT_HISTORY_TTL_SECS` от последнего обращения. Результаты
  инструментов не хранятся. Нет `conversation_id` — сервис создаёт UUID и
  возвращает его. Рестарт историю теряет.
- **Промпт** — [`prompts/chat.txt`](prompts/chat.txt) (`# version: chat-v3`),
  подстановки `{today}`, `{weekday}`, `{time}` (из `input.now`, иначе UTC
  сервера) и `{mcp_instructions}`. Нет файла — сервис не стартует.

**Провайдеры** (`src/chat_models.cpp`, адаптеры к одной нейтральной модели
сообщений):

- `lmstudio` — OpenAI-совместимый `POST {LLM_URL}/chat/completions` с
  `tools`; результаты — сообщения `role: tool`, `<think>` вырезается.
  Доступен, когда задан `LLM_URL` и последняя проба LLM `ok`.
- `claude` — Anthropic Messages API сырым HTTPS (`POST
  https://api.anthropic.com/v1/messages`, `anthropic-version: 2026-10-21`;
  у C++ нет официального SDK). Тело: `ANTHROPIC_MODEL`, `thinking:
  adaptive`, `output_config.effort` = `ANTHROPIC_EFFORT`, `tool_choice: auto`
  (в последнем вызове — `none`), без `temperature`/`top_p`. Серверный
  fallback при отказе (`fallbacks: "default"`, beta
  `server-side-fallback-2026-07-01`) включён, `ANTHROPIC_FALLBACKS=off`
  выключает. Ответ ассистента уходит обратно в историю цикла целиком,
  вместе с блоками `thinking` (append-only), все `tool_result` хода — одним
  сообщением `user`. Доступен, когда задан `ANTHROPIC_API_KEY` и пользователь
  в `ANTHROPIC_ALLOWED_USERS` (`user_id` через запятую или `*`).

Ключ провайдера определяется на каждый вызов модели (сейчас — ключ стека,
`owner: instance`); в задании, журнале и ответах API его нет.

### Настройки ИИ

`GET /api/ai/settings` — выбранный провайдер и список `providers` с
`available`/`reason`, `requires_consent`, `consented`, `consented_at`
(схема `AiSettings`). Без выбора — `AI_DEFAULT_PROVIDER` (`lmstudio`).

`PUT /api/ai/settings {provider, consent?}`:

- неизвестный или недоступный провайдер — `400 provider_unavailable`;
- `claude` без сохранённого или переданного `consent: true` — `409
  consent_required`, выбор не меняется;
- `{"provider":"claude","consent":false}` — отзыв: согласие стёрто, выбор —
  `lmstudio`;
- без `DATABASE_URL` (или база не отвечает) — `503 settings_unavailable`.

Хранится в базе ai (`migrations/000002_ai_user_settings`:
`ai_user_settings`, `ai_provider_consent`). Задание `chat` выполняется
провайдером, выбранным на момент создания; недоступен — `503
{"error":"provider_unavailable","provider":"…"}`, на другого провайдера
сервис молча не переключает.

**Приватность.** При выборе Claude результаты инструментов (суммы,
категории, комментарии операций) уходят в Anthropic — только после явного
согласия пользователя. Журнал и метрики чата содержат только id,
провайдера, имена инструментов, шаги, токены, длительности и исход — ни
текста вопроса и ответа, ни аргументов и результатов инструментов.

### Импорт выписки

Спецификация — `openspec/changes/add-statement-import`. Клиент — веб
(«ИИ» → «Импорт выписки»), бот выписки не принимает.

1. `POST /api/ai/imports {wallet_uuid, text | file, profile?}` — текст или
   файл `.pdf` (с текстовым слоем, `pdftotext -layout`), `.csv`, `.txt`
   (UTF-8, UTF-16 или Windows-1251) до 5 МБ в base64. Ответ сразу, разбор —
   в фоне. Скан — `failed: no_text`, текст без строк с суммой —
   `failed: no_operations`.
2. Текст режется на фрагменты (`AI_EXTRACT_CHUNK_CHARS`, по окну модели
   `LLM_CONTEXT`), каждый — фоновое задание `extract_rows`: модель возвращает
   операции с номерами строк, код проверяет, что сумма и дата буквально есть
   в этих строках (`src/statement/verify.cpp`). Не прошла — `check`, строки с
   суммой без операции — «не разобрано». CSV со встроенным профилем колонок
   (`[columns]`/`[types]`, Wirex) разбирается кодом без LLM.
3. Категории: правила пользователя (`merchant_rule`, растут из его правок),
   затем `classify_rows` — только описания продавцов и имена категорий,
   порог `AI_CLASSIFY_MIN_CONFIDENCE`.
4. `GET /api/ai/imports/{id}` в `ready` сверяет строки с операциями кошелька
   (`/api/reports/operations` токеном запроса) и отдаёт группы и готовые
   операции с `uuid` v5 от отпечатка. Клиент шлёт их в `POST /sync/push` и
   отмечает `POST …/committed`.

Профили формата — `prompts/banks/*.txt` (встроенные: `tbank` — PDF ТБанка,
`wirex` — CSV Wirex, `weavepay` — PDF Weave Pay (разбор слабый, см. design); секции `[signature]`, `[hints]`, `[examples]`, `[check]`
с `stop_words`/`date_formats`/`expense_word`/`transfer_words`, `[columns]`,
`[types]`, см. `src/statement/profiles.hpp`) и свои форматы пользователя:
пример добавляется из каждой правки суммы, даты или направления, ключ —
отпечаток строки заголовка колонок. Обезличенные образцы выписок —
`tests/statements/<bank>/`; прогон без сервиса — `wallet_ai_llm_try
--statement … --profile prompts/banks/<bank>.txt`.

Фоновые задания идут после интерактивных: трата текстом ждёт не больше
одного фрагмента. Импорт хранится в базе `ai` (`migrations/`, накат —
`wallet-infra/infra/db/migrate.sh ai up`): необработанный — 90 дней,
записанный (`done`) — 7. Без базы сервис работает, импорт отвечает 503, а
`features` в `/status` — без `statement_import`. Импорт переживает рестарт:
разобранные фрагменты не повторяются. В логах и метриках — только id,
статусы, счётчики и длительности.

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
| `LLM_CONTEXT` | `8192` | окно модели в LM Studio (`scripts/lms-load.sh`, `LMS_CONTEXT`); по нему режутся фрагменты выписки и считается `max_tokens` ответа |
| `LLM_API_KEY` | пусто | `Authorization: Bearer` к LLM |
| `LLM_TIMEOUT_MS` | `15000` | read-таймаут; connect — 2 с |
| `LLM_PROBE_INTERVAL_SECS` | `60` | как часто проверять LLM |
| `LLM_PROMPT_DIR` | `prompts` | где лежат промпты (`expense.txt`, `chat.txt`); нет файла — сервис не стартует |
| `WALLET_REPORT_URL` | — | wallet-report: справочник счетов токеном пользователя |
| `LLM_MAX_CONCURRENT` | `1` | воркеров локальной полосы = одновременных запросов к GPU |
| `AI_CLOUD_MAX_CONCURRENT` | `2` | воркеров облачной полосы (чат через Claude) |
| `LLM_REASONING_PARSE_EXPENSE` | `none` | `reasoning_effort` для `parse_expense`; `default` — не передавать |
| `LLM_MAX_TOKENS_PARSE_EXPENSE` | `512` | запас под рассуждения модели |
| `AI_JOB_TIMEOUT_SECS_PARSE_EXPENSE` | `60` | дедлайн генерации задания |
| `AI_JOB_TTL_SECS` | `600` | сколько хранить завершённое задание |
| `AI_QUEUE_MAX` | `20` | интерактивных заданий в очереди полосы; больше — `429 busy` |
| `AI_MAX_ACTIVE_PER_USER` | `3` | незавершённых заданий на пользователя |
| `LLM_MAX_PER_MINUTE` | `10` | новых заданий на пользователя в минуту |
| `DATABASE_URL` | пусто | база ai — импорт выписок; пусто — импорт выключен |
| `AI_DB_POOL_SIZE` | `4` | соединений с базой ai |
| `AI_EXTRACT_CHUNK_CHARS` | `1800` | фрагмент выписки, символов (без пустых строк и строк без цифр) |
| `AI_IMPORTS_MAX_ACTIVE` | `3` | импортов в работе всего (на пользователя — один) |
| `AI_IMPORT_TTL_DAYS` / `AI_IMPORT_DONE_TTL_DAYS` | `90` / `7` | срок необработанного и записанного импорта |
| `AI_IMPORT_PAUSE_MAX_MINS` | `60` | сколько ждать LLM, прежде чем `failed: unavailable` |
| `AI_CLASSIFY_MIN_CONFIDENCE` | `0.7` | ниже — категория только подсказкой (`suggestion`) |
| `AI_PROFILE_MAX_TOKENS` | `500` | бюджет профиля формата в промпте; фрагмент урезается, только если с профилем и ответом не влезает в `LLM_CONTEXT` |
| `AI_JOB_TIMEOUT_SECS_EXTRACT_ROWS` / `AI_JOB_TIMEOUT_SECS_CLASSIFY_ROWS` | `180` / `60` | дедлайн вызова LLM |
| `LLM_MAX_TOKENS_CLASSIFY_ROWS` | `1024` | у `extract_rows` — 200 + 100 на строку с суммой + символы / 6, не больше места в `LLM_CONTEXT` после промпта |
| `WALLET_MCP_URL` | пусто | wallet-mcp напрямую, не через гейтвей (`http://wallet-mcp:8088/api/mcp/`). Пусто — вид `chat` выключен |
| `AUTH_EXCHANGE_SECRET` | пусто | секрет actor `wallet-ai` для `/internal/token/exchange` google-auth (`TOKEN_EXCHANGE_SECRETS` там). Пусто — `chat` выключен |
| `AI_DEFAULT_PROVIDER` | `lmstudio` | провайдер без выбора пользователя; только локальный |
| `ANTHROPIC_API_KEY` | пусто | ключ стека для Claude. Пусто — `claude` недоступен (`not_configured`) |
| `ANTHROPIC_ALLOWED_USERS` | пусто | кому доступен Claude: `user_id` через запятую или `*`; пусто — никому |
| `ANTHROPIC_MODEL` | `claude-opus-5-5` | модель Claude (например, `claude-haiku-5-5` — дешевле) |
| `ANTHROPIC_MAX_TOKENS` | `16000` | `max_tokens` вызова Claude |
| `ANTHROPIC_EFFORT` | `low` | `output_config.effort` (`low` … `max`) |
| `ANTHROPIC_FALLBACKS` | вкл. | `off` — без серверного fallback при отказе модели |
| `AI_JOB_TIMEOUT_SECS_CHAT` | `120` | дедлайн всего задания `chat` |
| `AI_CHAT_MAX_STEPS` | `5` | вызовов модели с инструментами; затем один без них |
| `AI_CHAT_HISTORY_TURNS` / `AI_CHAT_HISTORY_TTL_SECS` | `6` / `1800` | пар «вопрос — ответ» в диалоге и их срок от последнего обращения |
| `AI_CHAT_TOOL_RESULT_MAX_CHARS` | `6000` | результат инструмента длиннее обрезается («…(обрезано)») |
| `LLM_MAX_TOKENS_CHAT` / `LLM_REASONING_CHAT` | `1024` / `none` | `max_tokens` и `reasoning_effort` чата через LM Studio; `default` — не передавать |
| `PORT` / `HOST` | `8087` / `127.0.0.1` | |
| `LOG_DIR` / `LOG_LEVEL` | stdout / debug | |

LLM необязательна: недоступна — сервис стартует и отвечает
`enabled=false`, клиенты работают без ИИ. Переменные чата тоже: пустые —
сервис стартует, `chat` отвечает 503, остальное работает как раньше. Для
чата LM Studio нужно окно `LLM_CONTEXT` не меньше 8192: схемы 6
инструментов, история и результаты в 4096 не помещаются.

## LLM на хосте (LM Studio)

GPU и модель — на хосте, вне docker. Проверено 2026-10-11 на GTX 1070 8 ГБ
с `google/gemma-4-e4b` (детали — «Шаг 9 подробно» в
`org/docs/TODO_telegram_bot.md`).

Пункты 1–2 и проверку из п. 4 делает `bash wallet-ai/scripts/lms-load.sh`:
запускает сервер на `0.0.0.0:1234`, если он не запущен, грузит модель под id
`wallet-parse`, если её нет, и проверяет `GET /v1/models`. Повторный запуск
ничего не меняет. Та же модель, загруженная под другим id (например, из GUI),
сперва выгружается — две копии в 8 ГБ не помещаются. Другая модель или id —
через переменные (`LMS_MODEL_KEY`, `LMS_IDENTIFIER`, ... — см. шапку
скрипта). Для загрузки токен не нужен; для проверки `GET /v1/models` скрипт
берёт токен LM Studio из `LLM_API_KEY`, а без него — из
`~/.config/wallet-secrets/prod/wallet-ai.env` (`LMS_SECRETS_FILE=...`). Нет
токена вовсе — проверка только по `lms ps`, с предупреждением.

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
  `ai_queue_interactive`, `ai_queue_background`, `ai_queue_cloud`;
- `ai_job_create_total{result}` у `chat` ещё: `provider_unavailable`,
  `invalid_token`;
- `ai_chat_tokens_total{provider,direction}` — токены вызовов модели
  (`input`/`output`), расход по провайдеру;
- `ai_chat_jobs_total{provider,result}` — задания `chat` по исходу (`done`
  или код отказа);
- `ai_chat_tool_calls_total{tool,result}` — вызовы инструментов: `ok`,
  `error` (`isError` wallet-mcp), `rpc_error`, `bad_args`, `unknown_tool`
  (с `tool="unknown"`);
- `ai_chat_steps` — гистограмма вызовов модели на задание;
- `ai_jobs_by_platform_total{platform}` — откуда задания (web, telegram, …);
- `ai_reports_errors_total{kind}` — ошибки запросов к wallet-report;
- `jwt_claims_total{result}` — проверки iss/aud/scope;
- `ai_statement_imports_total{status}` — импорты по итогу (`ready`, `done`,
  `failed`, `cancelled`);
- `ai_statement_rows_total{result}` — строки импортов: `verified`,
  `check_failed`, `unparsed` — качество извлечения на живых выписках;
- `ai_statement_chunk_duration_seconds{result}` — разбор одного фрагмента;
- `http_requests_total`, `http_request_duration_seconds` — как у всех сервисов.

## Версия

`GET /version`, строка лога при старте и `build_info` в `GET /metrics` —
общая схема C++-сервисов проекта, см. [README google-auth, «Версия»](https://github.com/WalletTransactions/google-auth#версия).
Там же — `build` (тег образа из `--build-arg APP_BUILD`) и `started_at`
(время запуска) в `GET /version`.

Релиз — git-тегами: `vX.Y.Z-rc.N` публикует образ в ghcr и выкатывает его
в test, `vX.Y.Z` на том же коммите — тот же образ в prod; `X.Y.Z` должен
совпадать с `VERSION`. Подробно — [README google-auth, «Версия»](https://github.com/WalletTransactions/google-auth#версия), «Релиз».
