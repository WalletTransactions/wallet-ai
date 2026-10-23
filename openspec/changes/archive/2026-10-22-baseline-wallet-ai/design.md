# Design: базовая спецификация wallet-ai

## Context

Мотивация — в `proposal.md`. Спецификации в `specs/` восстановлены по коду на
коммите `a1f098f` (main, 2026-10-11), вендоренному wallet-common `0.1.0
(a53e37ba)`, тестам `tests/` (44 юнит-теста), `prompts/expense.txt` (`expense-v2`),
`openapi.yaml`, `README.md`, `.env.example`, `scripts/lms-load.sh`. Обоснования
сверялись с `org/docs/TODO_wallet_ai.md` и «Шаг 9 подробно»
`org/docs/TODO_telegram_bot.md`.

При расхождении документа и кода источником базы считался **код**; расхождения
вынесены в «Наблюдения», а не исправлены в спецификации.

Сверка (tasks.md, 2026-10-22) — на коммите `be6133c`. После `a1f098f` в коде
изменился только комментарий в вендоренном `util/jwt_claims.hpp` (wallet-common
`d7dc2e14`), поведение то же. Юнит-тесты (`docker build --target test`): 44, упало 0.

## Goals / Non-Goals

**Goals:**
- Описать наблюдаемое поведение так, чтобы каждый сценарий проверялся юнит-тестом
  (`FakeLlm`, `Store` с подменой часов) или ручным прогоном через гейтвей.
- Сохранить обоснование решений, которое сейчас живёт в плане и комментариях.

**Non-Goals:**
- Контракт wallet-common как библиотеки: описано только то, что видно в ответе
  `parse_expense` (проверки, resolver).
- Нереализованное по плану: импорт выписок, `classify_rows`, `chat`,
  `template_draft`, `/api/ai/imports`, `/api/ai/mcp`, SSE, eval, автозапуск LM
  Studio, вторая реплика.
- Исправление наблюдений и любое изменение поведения.

## Decisions

### D1. Всё, что зовёт LLM, — асинхронные задания
Генерация на GTX 1070 занимает 2–13 с, иногда больше. Синхронный запрос упирался
бы в 30 с таймаута гейтвея и в единственный поток поллинга бота. Задание +
long-poll (≤ 25 с) снимают оба ограничения без SSE. Источник: план §3, §5.

### D2. Очередь в памяти, воркеров столько, сколько запросов выдержит GPU
`LLM_MAX_CONCURRENT=1` — семафор на одну видеокарту. Интерактивная очередь важнее
фоновой, чтобы трата пользователя не ждала будущий импорт. Хранение в памяти:
рестарт теряет задания, и это допустимо для коротких разборов; база или Redis —
со второй репликой. Источник: план §4, §5.

### D3. Модель извлекает, код проверяет
LLM отдаёт строки полей по strict JSON-схеме с `enum` имён пользователя (llama.cpp
превращает схему в грамматику). Сумма обязана встречаться в тексте, дата — не в
будущем и не старше года; выбор счетов — тот же resolver, что у строгого формата
бота. Клиенту приходит уже проверенный черновик. Источник: план бота, 9c.5.

### D4. Справочник счетов — при создании задания
Токен бота живёт 5 минут и может истечь, пока задание ждёт в очереди. Поэтому
`reports/accounts` читается в хендлере создания, и в задание кладётся ответ, а не
токен. Источник: план §3.

### D5. Сервис ничего не пишет в кошелёк
Токены, с которыми wallet-ai ходит в другие сервисы, дают только чтение; запись —
у клиента после подтверждения. Ошибка модели не может стать операцией.

### D6. Промпт — файл с версией
`prompts/expense.txt` меняется без пересборки, `# version:` попадает в логи и
результат, чтобы eval и разбор инцидентов знали, какой промпт отвечал.

## Наблюдения

- Р1. README, «Состояние» (`README.md:18–21`): «Бот и веб к заданиям ещё не
  подключены (шаги 4–5)» — бот подключён (шаг 4, `TODO_wallet_ai.md` §10).
- Р2. README, таблица конфигурации: `LLM_TIMEOUT_MS` описан как read-таймаут
  вызовов LLM, а в коде он действует только на пробу `GET /models`
  (`llm_probe.cpp`); разбор траты использует
  `AI_JOB_TIMEOUT_SECS_PARSE_EXPENSE` (`llm_client.cpp:358–359`).
- Р3. `AI_JOB_TIMEOUT_SECS_PARSE_EXPENSE` — дедлайн HTTP-запроса к LLM, а не
  всего задания, как в плане §5: время в очереди не ограничено
  (`main.cpp:133`, `llm_client.cpp:359`).
- Р4. `.env.example` не содержит обязательного `WALLET_REPORT_URL` (без него
  сервис не стартует, `main.cpp:123–124`), а также `AI_*`,
  `LLM_REASONING_*`, `LLM_MAX_TOKENS_*`, `LLM_MAX_CONCURRENT`,
  `LLM_PROMPT_DIR`.
- Р5. Воркер вызывает `parse_expense::run` для любого задания, не глядя на
  `kind` (`ai.cpp:171`); диспетчеризации видов нет — сейчас безопасно, потому
  что `handle_create` пропускает только `parse_expense` (`ai.cpp:52`).
- Р6. Фоновая очередь есть, но ни один вид её не использует; лимит
  `AI_QUEUE_MAX` распространяется только на интерактивную
  (`jobs.cpp:65`) — фоновая не ограничена.
- Р7. Отменённое выполняющееся задание сразу становится `cancelled` и перестаёт
  считаться в `too_many_active` (`jobs.cpp:130`, `jobs.cpp:41`), хотя GPU ещё
  занят его генерацией.
- Р8. `failed` с причинами `unauthorized`/`model_not_loaded` от LLM сводится к
  `unavailable` (`parse_expense.cpp:143–145`); различить можно только по логу
  (`llm_status`).
- Р9. План §3 перечисляет статус `expired`; в коде истёкшее задание — 404, а в
  `openapi.yaml` `expired` нет — документ плана устарел (`jobs.cpp:164–175`).
- Р10. `ai_jobs_by_platform_total` пишет `web` для токена без `platform`
  (`ai.cpp:85`) — токен от телефона без claim попадёт в «web».
- Р11. `resolution.wallet.kind` при `currency_mismatch` — `ask` с `force`, а не
  `reject` (`expense/resolver.hpp:61`); `openapi.yaml` перечисляет причины без
  разбивки по `kind`, что легко прочитать неверно.
- Р12. `job_id` не вида UUID (`[0-9a-fA-F-]{36}`) не совпадает с маршрутом, и
  `GET`/`DELETE` получают 404 от cpp-httplib с пустым телом, а не JSON
  «задания нет…» (`ai.cpp:153–156`). `wait` разбирается `std::stoi`, поэтому
  `wait=3abc` принимается как 3 (`ai.cpp:102`).
- Р13. `LLM_REASONING_PARSE_EXPENSE=none` не гарантирует отсутствие рассуждений:
  на ручном прогоне `gemma-4-e4b` на «обед 1500 25 декабря» потратила 489 из 512
  `max_tokens` на reasoning, ответ оборвался (`finish_reason_length` →
  `failed bad_response`, 15 с). Код ведёт себя по спецификации
  (`llm_client.cpp:293–295`), но запаса `LLM_MAX_TOKENS_PARSE_EXPENSE` (512) на
  такие фразы не хватает (`main.cpp:132`).
- Р14. Через гейтвей запрос без токена или с битым токеном отклоняет сам гейтвей
  (401 с его текстом «gateway: access-токен отсутствует…»), до wallet-ai он не
  доходит. Текст 401 из `ai-access` видит только клиент, который ходит напрямую
  (бот) (`ai.cpp:23`).

## Ручной прогон (2.6)

2026-10-22, гейтвей `:8090`, LM Studio `google/gemma-4-e4b` как `wallet-parse`
(контекст 4096), промпт `expense-v2`, cookie `session` одного пользователя.

| Проверка | Ответ | Сценарий |
| --- | --- | --- |
| `GET status` без модели / с моделью | `enabled:false, reason:model_not_loaded` / `enabled:true, features:[parse_expense]` | совпадает |
| без токена | 401 от гейтвея | см. Р14 |
| `kind: chat`; `input` без `now` | 400, тексты дословно | совпадает |
| «вчера такси 450» | `draft`, `2026-10-21 12:00:00`, категория `found`, кошелёк `ask not_given`, 4,3 с | совпадает |
| «привет, как дела» | `not_expense`, 8,1 с | совпадает |
| «кофе 200 и булка 150»; «такси двести рублей»; «продукты 0» | `rejected`: `amount_not_in_text`, `amount_not_in_text`, `zero_amount` | совпадает |
| «обед 1500 25 декабря» | `failed bad_response`, `finish_reason_length` | совпадает, см. Р13 |
| отмена выполняющегося | 204, `GET` сразу `cancelled`; повторная отмена — 404 | совпадает |
| несуществующий UUID; `wait=abc`; `job_id` не UUID | 404 JSON; 400; 404 пустое тело | совпадает; см. Р12 |
| снимок задания | `input` нет | совпадает |
| лог `job_finished` | `result`, токены, длительности, без текста | совпадает |

Не проверено вживую: «Чужое задание» (нужен второй пользователь) — код отдаёт
тот же 404, что для несуществующего (`jobs.cpp:100`), это покрыто
`jobs_owner_only_and_input_hidden`. Модель увела «послезавтра кино 600» в
сегодняшнюю дату (`draft`), поэтому `future_date` вживую не получен, только в
`check_dates`.

## Покрытие

Сценарии сверены с кодом: расхождения в коде не найдены. Неточности спецификации
исправлены в `expense-parsing`: время прошлой даты (`12:00:00`), какие валюты
узнаются, «…» при обрезке комментария, полный список причин `unavailable`.

Тест на сценарий (`tests/*.cpp`):

| Capability | Сценарий | Тест |
| --- | --- | --- |
| ai-access | Токен из cookie; Нет токена; Токен для другого сервиса; Нет ai:use; Токен без aud | нет теста (`access.cpp`, `session.cpp`; `jwt_claims` — в wallet-common) |
| ai-access | Проверка живости | частично: `required_scope_only_for_api_ai` |
| ai-status | Модель загружена | `parse_models_finds_model` (метрика — нет) |
| ai-status | Неверный ключ LM Studio | `parse_models_errors` (текст лога — нет) |
| ai-status | Стабильное состояние; Старт без LM Studio | нет теста (`main.cpp::set_llm_status`, `main.cpp`) |
| ai-status | ИИ выключен; ИИ включён | `status_disabled_hides_model_and_gives_reason`, `status_enabled_when_probe_ok` |
| ai-jobs | Новое задание; Неизвестный вид; LM Studio остановлен; нечисловой `wait` | нет теста (хендлеры `ai.cpp`) |
| ai-jobs | Повтор бота | `jobs_idempotency_key_returns_same_job` (метрика `reused` — нет) |
| ai-jobs | Четвёртое незавершённое | `jobs_limits` |
| ai-jobs | Интерактивное раньше фонового | `jobs_interactive_before_background` |
| ai-jobs | Завершённое задание; Чужое задание | `jobs_complete_drops_input_and_keeps_result_until_ttl`, `jobs_owner_only_and_input_hidden` |
| ai-jobs | Ответ по готовности; Ответ по истечении | `jobs_long_poll_returns_on_completion_and_on_timeout` |
| ai-jobs | Отмена во время генерации | `jobs_cancel_queued_and_running` |
| ai-jobs | Результат истёк | `jobs_complete_drops_input_and_keeps_result_until_ttl` |
| expense-parsing | Нет now | `validate_input_rules` (текст ошибки не сверяется) |
| expense-parsing | Токен истёк к запуску | нет теста (`ai.cpp::handle_create`, `make_job_input`) |
| expense-parsing | Чужое имя невозможно | `request_body_enums_include_empty_and_user_names` |
| expense-parsing | Долгая генерация | `run_not_expense_and_failures` (через `FakeLlm`; дедлайн HTTP — нет) |
| expense-parsing | Оборванный JSON | `parse_bad_responses` |
| expense-parsing | Приветствие | `parse_empty_amount_is_not_expense`, `run_not_expense_and_failures` |
| expense-parsing | Модель сложила суммы | `check_amount_must_be_in_text`, `run_rejected_when_amount_not_in_text` |
| expense-parsing | Будущая дата; Вчерашняя трата | `check_dates` |
| expense-parsing | Неизвестная валюта | `check_currency_only_known` |
| expense-parsing | Кошелёк по умолчанию; Валюта не совпала с кошельком | нет теста в wallet-ai (resolver wallet-common) |
| service-operations | Промпт без версии | `parse_prompt_rules` |
| service-operations | Завершение задания | частично: `run_log_has_no_text_or_amounts` (`job_finished` — нет) |
| service-operations | Нет LLM_MODEL; Запуск в контейнере; LLM недоступна; Задание от бота; Исключение | нет теста (`main.cpp`, `docs.cpp`) |

Пробелы — хендлеры `ai.cpp`, проверка токена и `main.cpp`: юнит-тестов на
HTTP-уровень нет. Закрывать отдельным change, не этим.

## Risks / Trade-offs

- [Спецификация восстановлена по коду] → неудачное поведение (Р3, Р5–Р8) могло
  войти в требования. Митигация: при сверке такие места помечаются в
  «Наблюдениях», а не закрепляются.
- [Сценарии с живой LLM] → проверяются только руками (`wallet_ai_llm_try`) и
  зависят от модели. Митигация: требования формулируют поведение кода вокруг
  модели, а не качество её ответов.
