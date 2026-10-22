# Spec Delta

## Purpose

Как сервис запускается, настраивается и наблюдается: переменные окружения,
промпт из файла, служебные ручки, метрики и логи, в которые не попадают тексты
пользователей.

## ADDED Requirements

### Requirement: Обязательная конфигурация
Сервис SHALL не стартовать (код выхода 1, ошибка в лог) без `AUTH_SERVICE_URL` и
`WALLET_REPORT_URL`, при заданном `LLM_URL` без `LLM_MODEL`, при `LLM_URL` не вида
`http://host:port/путь` и без файла промпта.

#### Scenario: Нет LLM_MODEL
- **WHEN** задан `LLM_URL`, но не задан `LLM_MODEL`
- **THEN** сервис пишет «LLM_MODEL must be set when LLM_URL is set …» и завершается с кодом 1

### Requirement: Промпт из файла
System-промпт SHALL читаться при старте из `{LLM_PROMPT_DIR}/expense.txt` (по
умолчанию `prompts/`). Строки с `#` — комментарии; строка `# version: X` обязательна
и даёт `prompt_version`. Правка промпта требует только рестарта.

#### Scenario: Промпт без версии
- **WHEN** в файле нет строки `# version:`
- **THEN** сервис не стартует с ошибкой «в промпте нет строки "# version: ..."»

### Requirement: Настройки по умолчанию
Без переменных окружения сервис SHALL слушать `127.0.0.1:8087`, читать `.env` из
рабочего каталога и писать логи в stdout (`LOG_DIR` пуст, `stdout` или `-`), иначе —
в файлы по дням в `LOG_DIR`.

#### Scenario: Запуск в контейнере
- **WHEN** compose задаёт `HOST=0.0.0.0`
- **THEN** сервис доступен другим контейнерам на порту 8087

### Requirement: Служебные ручки
Сервис SHALL отдавать `GET /healthz` (`{"status":"ok"}`), `GET /metrics`
(Prometheus), `GET /version` (сервис, версия, git-коммит), `GET /openapi.yaml` и
`GET /docs` (Swagger UI) без токена. `/healthz` не зависит от состояния LLM.

#### Scenario: LLM недоступна
- **WHEN** LM Studio остановлен
- **THEN** `/healthz` отвечает 200, а недоступность видна в `/api/ai/status` и `llm_up`

### Requirement: Логи без пользовательских данных
Сервис SHALL NOT писать в лог тела запросов, тексты трат, ответы модели и
результаты. В лог задания — `job_id`, `kind`, исход, причины проверок, токены,
длительности, версия промпта. `trace_id` берётся из `X-Trace-Id` и возвращается в
ответе.

#### Scenario: Завершение задания
- **WHEN** задание `parse_expense` завершилось `draft`
- **THEN** в лог пишется `job_finished` с `result: draft`, `queue_ms`, `run_ms`, `completion_tokens`, без текста траты

### Requirement: Метрики
Сервис SHALL публиковать `llm_up`, `ai_job_create_total{result}`,
`ai_job_result_total{result}`, `ai_job_duration_seconds{kind}`,
`ai_queue_wait_seconds{kind}`, `ai_queue_interactive`, `ai_queue_background`,
`ai_jobs_by_platform_total{platform}`, `ai_reports_errors_total{kind}`,
`jwt_claims_total{result}` и общие HTTP-метрики.

#### Scenario: Задание от бота
- **WHEN** бот (токен с `platform: telegram`) создаёт задание
- **THEN** растёт `ai_jobs_by_platform_total{platform="telegram"}`

### Requirement: Ошибки обработчиков
Необработанное исключение в хендлере SHALL давать 500 `{"error": "internal server
error"}` и запись в лог с методом и путём.

#### Scenario: Исключение
- **WHEN** хендлер бросил исключение
- **THEN** клиент получает 500 без деталей исключения
