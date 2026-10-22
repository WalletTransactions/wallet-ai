# Tasks

Перед применением заархивировать `baseline-wallet-ai`. Сборка и тесты — в
Docker, по одному сервису за раз (`docker compose ... --env-file
wallet-infra/infra/db/.env`). Юнит-тесты — `tests/` с `FakeLlm`.

## 1. База `ai` (wallet-infra + wallet-ai)

- [ ] 1.1 wallet-infra `init-db.sh`: роль и база `ai` (`AI_POSTGRES_USER/PASSWORD/DB`), DML-права по `ALTER DEFAULT PRIVILEGES` как у telegram; `infra/db/.env.example`; проверка — чистый `docker compose up db` создаёт базу и роль
- [ ] 1.2 wallet-infra `migrate.sh`: цель `ai` → `wallet-ai/migrations/`; проверка — `bash infra/db/migrate.sh ai up` и `down` отрабатывают
- [ ] 1.3 wallet-ai `migrations/000001_init.*.sql`: `statement_import`, `statement_chunk`, `statement_row`, `merchant_rule`, `statement_imported` с индексами по `user_id`, `expires_at`, `(user_id, fingerprint)`, `(user_id, pattern)`; проверка — миграция вверх-вниз-вверх без ошибок
- [ ] 1.4 wallet-ai `src/db.*` (libpq, пул соединений по образцу wallet-telegram), `AI_POSTGRES_*` в `main.cpp`, `.env.example`; без базы — старт с выключенным импортом; проверка — юнит-тест конфигурации; при остановленной базе `/status` без `statement_import`
- [ ] 1.5 compose и k8s (`10-wallet-ai.yaml`): переменные базы, `depends_on: db`; проверка — `docker compose up wallet-ai` поднимается healthy

## 2. Задания: диспетчеризация и внутренние фоновые

- [ ] 2.1 `run_worker` — обработчик по `kind`, неизвестный — `failed: internal` (закрывает Р5 baseline); проверка — юнит-тест
- [ ] 2.2 `Store::create_internal` для `extract_rows`/`classify_rows` в фоновую очередь мимо пользовательских лимитов; `POST /api/ai/jobs` с этими `kind` — 400 `unknown_kind`; проверка — юнит-тесты: импорт не трогает `too_many_active`, интерактивное задание обгоняет фоновое

## 3. Приём импорта

- [ ] 3.1 `POST /api/ai/imports`: JSON-вход, `one_source`, `empty`, `unknown_wallet` (по `/api/reports/accounts` токеном пользователя), `accounts_snapshot`, `set_payload_max_length(8 МБ)`; проверка — юнит-тесты на каждый отказ
- [ ] 3.2 Извлечение текста: base64 → PDF через `pdftotext -layout - -` (pipe, `setrlimit` 256 МБ, 20 с) и `no_text`; CSV/TXT — UTF-8 или Windows-1251 (iconv); 5 МБ, `unsupported_file`; `poppler-utils` в runtime-образе; проверка — тесты на образцах: PDF с текстом, PDF-скан, CSV в cp1251
- [ ] 3.3 Пределы 300 000 символов и 80 фрагментов → 413 `too_large`; один импорт в работе на пользователя (429 `import_in_progress`), `AI_IMPORTS_MAX_ACTIVE`; проверка — юнит-тесты

## 4. Извлечение строк

- [ ] 4.1 `statement/chunk.*`: нумерация, нарезка по `AI_EXTRACT_CHUNK_CHARS` на границах дат и пустых строк, `date_context`; проверка — тесты на многострочных операциях и заголовках дат
- [ ] 4.2 `prompts/extract_rows.txt` (`# version: extract-v1`), `llm_client::extract_rows` со строгим `json_schema`, `max_tokens` по числу строк; проверка — тест `request_body` и `parse_response`; ручной прогон `wallet_ai_llm_try` на обезличенной выписке
- [ ] 4.3 `statement/verify.*`: нормализация сумм, поиск даты (форматы спеки, год из периода, будущее и старше 3 лет), знак из исходника перекрывает `dir`, `currency_mismatch`; проверка — табличные юнит-тесты
- [ ] 4.4 Непокрытые строки (денежная сумма без ссылки, без итогов «Итого/Остаток/Баланс»), схлопывание по `lines`, повтор пополам при битом ответе; проверка — тесты с `FakeLlm`, который пропускает и дублирует строки
- [ ] 4.5 `fingerprint` и `op_uuid` (D8); проверка — тест: та же выписка даёт те же значения, две одинаковые покупки — разные

## 4a. Профили формата

- [ ] 4.6 Обезличенные образцы выписок 2–3 основных банков (PDF и CSV) в `tests/statements/<bank>/`; проверка — суммы и описания заменены, структура колонок сохранена
- [ ] 4.7 `statement/profiles.*`: разбор `prompts/banks/*.txt` (секции `[signature]`, `[hints]`, `[examples]`, `[check]`), битый файл пропускается с `profile_invalid`; встроенные профили по образцам из 4.6; проверка — тест загрузки каждого файла профиля и битого файла
- [ ] 4.8 Отпечаток формата (строка заголовка колонок, словарь, нормализация) и автоопределение (`auto`, `none`, `ambiguous`); `profile` в `POST /imports`, `unknown_profile`; проверка — тесты: CSV за два месяца дают один отпечаток; две совпавшие сигнатуры — `ambiguous`
- [ ] 4.9 Профиль в промпте `extract_rows` с бюджетом `AI_PROFILE_MAX_TOKENS`, уменьшение фрагмента на бюджет; `[check]` в проверке (стоп-слова, форматы даты, знак расхода перекрывает `dir`); проверка — тест сборки промпта (длина ≤ контекста) и тест «остаток против суммы» с `FakeLlm`
- [ ] 4.10 Свои форматы: `format_profile`/`format_example` в миграции 1.3, пример из `PATCH rows` при правке суммы, даты или направления, 5 последних, свой формат поверх встроенного; `GET/PATCH/DELETE /api/ai/statement-profiles*`, 403 на встроенный, 404 на чужой; проверка — юнит-тесты; повторная выписка того же формата получает свои примеры
- [ ] 4.11 Сравнение на образцах из 4.6 с профилем и без (`wallet_ai_llm_try`): доля `verified`, `check_failed`, `unparsed`; результат — в `design.md` → «Наблюдения после прогона»; проверка — у профиля качество не хуже, чем без него

## 5. Категории

- [ ] 5.1 `statement/pattern.*` — нормализация описания; проверка — тесты на типичные описания (`YANDEX*GO MOSCOW RUS 4021` → `YANDEX*GO`)
- [ ] 5.2 Правила `merchant_rule`, затем пачки `classify_rows` по 20 `pattern` (`prompts/classify_rows.txt`, `enum` категорий + `""`), порог `AI_CLASSIFY_MIN_CONFIDENCE`, `suggestion`; при сбое — `ready` без категорий; проверка — тесты с `FakeLlm`

## 6. Планировщик импорта

- [ ] 6.1 `ImportRunner`: одно задание на импорт в очереди, круговой обход, пауза при неуспешной пробе LLM, `AI_IMPORT_PAUSE_MAX_MINS`, три таймаута фрагмента → `failed: timeout`, `retry`; проверка — тесты на чередование A1 B1 A2 и на паузу
- [ ] 6.2 Возобновление при старте по `statement_chunk.status`; проверка — интеграционно: `docker compose restart wallet-ai` посреди разбора, импорт доходит до `ready` без повторных фрагментов
- [ ] 6.3 `expires_at` = создание + `AI_IMPORT_TTL_DAYS` (90), при `done` — min(сейчас + `AI_IMPORT_DONE_TTL_DAYS` (7), создание + 90); чистка раз в час; `DELETE` с отменой заданий; проверка — тест с подменённым временем: `ready` на 89-й день жив, на 91-й удалён; `done` удалён через 7 дней, `statement_imported` остался

## 7. Проверка и запись

- [ ] 7.1 `GET /api/ai/imports` и `GET /api/ai/imports/{id}`: группы, строки с источниками, готовые операции, 404 на чужой id; проверка — юнит-тесты на группировку
- [ ] 7.2 Сверка с внесёнными (D7) через `/api/reports/operations` с курсором, ±1 день, «одна операция — одна строка», `statement_imported`, кэш на минуту, `?reconcile=1`, `reconciled: false` при сбое wallet-report; проверка — тесты с фейковым `reports_client`
- [ ] 7.3 `PATCH /rows` (категория → правило и соседние строки, сумма и дата снимают `check`, `decision`), `POST /rows` из «не разобрано», `POST /committed` → `statement_imported`, статус `done`; проверка — юнит-тесты

## 8. Наблюдаемость и документация

- [ ] 8.1 Метрики `ai_statement_imports_total{status}`, `ai_statement_rows_total{result}`, `ai_statement_chunk_duration_seconds`; логи без текста и сумм; проверка — `/metrics` после прогона, `grep` логов на отсутствие сумм
- [ ] 8.2 `openapi.yaml` (все `/api/ai/imports*`, `/api/ai/statement-profiles*`, `features`), README (раздел «Импорт выписки», переменные окружения); проверка — `/docs` открывается, схемы совпадают с ответами
- [ ] 8.3 `org/docs/TODO_statement_import.md` и `TODO_wallet_ai.md` §10 шаг 6 — отступление от §1 (LLM извлекает строки) и выбор варианта «В»; проверка — документы ссылаются на этот change

## 9. Живой прогон

- [ ] 9.1 Три настоящие выписки (PDF одного банка, CSV другого, вставленный текст) через `curl` с токеном: доля `verified`, `check_failed`, `unparsed` записана в `design.md` → «Наблюдения после прогона»; проверка — суммы `ready` сходятся с итогом выписки за вычетом групп
- [ ] 9.2 Трата текстом во время импорта ждёт не дольше одного фрагмента; проверка — замер `queue_ms`
- [ ] 9.3 `openspec validate add-statement-import --strict`; проверка — change валиден
