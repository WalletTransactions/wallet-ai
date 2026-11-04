## MODIFIED Requirements

### Requirement: Создание задания
`POST /api/ai/jobs` с `{kind, input, idempotency_key?}` SHALL отвечать 202
`{job_id, status}`. Тело не JSON-объект — 400; `kind` не `parse_expense` и
не `chat` — 400 «неизвестный kind: …»; `input` не прошёл проверку вида — 400
с текстом проверки. `job_id` — UUID v4.

#### Scenario: Новое задание
- **WHEN** пользователь создаёт задание `parse_expense` с корректным `input`
- **THEN** ответ 202 `{"job_id": "<uuid>", "status": "queued"}`

#### Scenario: Неизвестный вид
- **WHEN** `kind: "summarize"`
- **THEN** ответ 400 `{"error": "неизвестный kind: summarize"}`

#### Scenario: Задание chat
- **WHEN** пользователь создаёт задание `chat` с корректным `input`
- **THEN** ответ 202 `{"job_id": "<uuid>", "status": "queued"}`

### Requirement: ИИ выключен — без задания
Если провайдер, которым будет выполняться задание, недоступен, `POST
/api/ai/jobs` SHALL сразу отвечать 503, не создавая задания: для
`parse_expense` (всегда `lmstudio`) — `{"error": "unavailable"}`, когда
последняя проба LLM не `ok`; для `chat` — по требованию «Провайдер задания
фиксируется при создании» (`ai-providers`).

#### Scenario: LM Studio остановлен
- **WHEN** проба дала `unreachable`, и клиент создаёт задание `parse_expense`
- **THEN** ответ 503, метрика `ai_job_create_total{result="unavailable"}` растёт

#### Scenario: LM Studio остановлен, чат через Claude
- **WHEN** проба дала `unreachable`, а у пользователя выбран `claude`
- **THEN** задание `chat` создаётся

### Requirement: Очередь и воркеры
Задания SHALL идти по двум полосам: локальная (`lmstudio`, все
`parse_expense` и импорт) — `LLM_MAX_CONCURRENT` воркеров (по умолчанию 1),
облачная (`claude`) — `AI_CLOUD_MAX_CONCURRENT` воркеров (по умолчанию 2).
Воркер локальной полосы берёт задание из интерактивной очереди, а из
фоновой — только когда интерактивная пуста. Внутри очереди задания идут по
порядку создания. Взятое задание переходит в `running`. Предел `AI_QUEUE_MAX`
считается для каждой полосы отдельно.

#### Scenario: Интерактивное раньше фонового
- **WHEN** в фоновой очереди есть задание, а затем создаётся интерактивное
- **THEN** свободный воркер берёт интерактивное первым

#### Scenario: Claude не ждёт GPU
- **WHEN** локальный воркер занят `parse_expense`, и пользователь с выбранным `claude` создаёт `chat`
- **THEN** задание `chat` переходит в `running` без ожидания локального воркера
