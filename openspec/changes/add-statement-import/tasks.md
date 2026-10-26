# Tasks

Перед применением заархивировать `baseline-wallet-ai`. Сборка и тесты — в
Docker, по одному сервису за раз (`docker compose ... --env-file
wallet-infra/infra/db/.env`). Юнит-тесты — `tests/` с `FakeLlm`.

## 1. База `ai` (wallet-infra + wallet-ai)

- [x] 1.1 wallet-infra `init-db.sh`: роль и база `ai` (`AI_POSTGRES_USER/PASSWORD/DB`), DML-права по `ALTER DEFAULT PRIVILEGES` как у telegram; `infra/db/.env.example`; проверка — чистый `docker compose up db` создаёт базу и роль
- [x] 1.2 wallet-infra `migrate.sh`: цель `ai` → `wallet-ai/migrations/`; проверка — `bash infra/db/migrate.sh ai up` и `down` отрабатывают
- [x] 1.3 wallet-ai `migrations/000001_init.*.sql`: `statement_import`, `statement_chunk`, `statement_row`, `merchant_rule`, `statement_imported` с индексами по `user_id`, `expires_at`, `(user_id, fingerprint)`, `(user_id, pattern)`; проверка — миграция вверх-вниз-вверх без ошибок
- [x] 1.4 wallet-ai `src/db.*` (libpq, пул соединений по образцу wallet-telegram), `AI_POSTGRES_*` в `main.cpp`, `.env.example`; без базы — старт с выключенным импортом; проверка — юнит-тест конфигурации; при остановленной базе `/status` без `statement_import` (отступление: `DATABASE_URL` вместо `AI_POSTGRES_*`, design «Реализация»; проверено 2026-10-23: юнит-тест `status_json` с базой и без, контейнер с недоступной базой стартует, пишет `db_unavailable` и отвечает на запросы)
- [x] 1.5 compose и k8s (`10-wallet-ai.yaml`): переменные базы, `depends_on: db`; проверка — `docker compose up wallet-ai` поднимается healthy

## 2. Задания: диспетчеризация и внутренние фоновые

- [x] 2.1 `run_worker` — обработчик по `kind`, неизвестный — `failed: internal` (закрывает Р5 baseline); проверка — юнит-тест
- [x] 2.2 `Store::create_internal` для `extract_rows`/`classify_rows` в фоновую очередь мимо пользовательских лимитов; `POST /api/ai/jobs` с этими `kind` — 400 `unknown_kind`; проверка — юнит-тесты: импорт не трогает `too_many_active`, интерактивное задание обгоняет фоновое

## 3. Приём импорта

- [x] 3.1 `POST /api/ai/imports`: JSON-вход, `one_source`, `empty`, `unknown_wallet` (по `/api/reports/accounts` токеном пользователя), `accounts_snapshot`, `set_payload_max_length(8 МБ)`; проверка — юнит-тесты на каждый отказ
- [x] 3.2 Извлечение текста: base64 → PDF через `pdftotext -layout - -` (pipe, `setrlimit` 256 МБ, 20 с) и `no_text`; CSV/TXT — UTF-8 или Windows-1251 (iconv); 5 МБ, `unsupported_file`; `poppler-utils` в runtime-образе; проверка — тесты на образцах: PDF с текстом, PDF-скан, CSV в cp1251
- [x] 3.3 Пределы 300 000 символов и 80 фрагментов → 413 `too_large`; один импорт в работе на пользователя (429 `import_in_progress`), `AI_IMPORTS_MAX_ACTIVE`; проверка — юнит-тесты

## 4. Извлечение строк

- [x] 4.1 `statement/chunk.*`: нумерация, нарезка по `AI_EXTRACT_CHUNK_CHARS` на границах дат и пустых строк, `date_context`; проверка — тесты на многострочных операциях и заголовках дат
- [x] 4.2 `prompts/extract_rows.txt` (`# version: extract-v1`), `llm_client::extract_rows` со строгим `json_schema`, `max_tokens` по числу строк; проверка — тест `request_body` и `parse_response`; ручной прогон `wallet_ai_llm_try` на обезличенной выписке (сейчас extract-v3, лимит — от окна, 10.3; тест `rows_request_and_response`; прогоны на синтетике, Wirex и ТБанке — design «Наблюдения»)
- [x] 4.3 `statement/verify.*`: нормализация сумм, поиск даты (форматы спеки, год из периода, будущее и старше 3 лет), знак из исходника перекрывает `dir`, `currency_mismatch`; проверка — табличные юнит-тесты
- [x] 4.4 Непокрытые строки (денежная сумма без ссылки, без итогов «Итого/Остаток/Баланс»), схлопывание по `lines`, повтор пополам при битом ответе; проверка — тесты с `FakeLlm`, который пропускает и дублирует строки
- [x] 4.5 `fingerprint` и `op_uuid` (D8); проверка — тест: та же выписка даёт те же значения, две одинаковые покупки — разные

## 4a. Профили формата

- [x] 4.6 Обезличенные образцы выписок 2–3 основных банков (PDF и CSV) в `tests/statements/<bank>/`; проверка — суммы и описания заменены, структура колонок сохранена. Есть: Wirex CSV (`tests/statements/wirex/`) и ТБанк PDF двух счетов (`tests/statements/tbank/`, текст после `pdftotext -layout` — 11.5); третий банк — по мере появления выписок
- [x] 4.7 `statement/profiles.*`: разбор `prompts/banks/*.txt` (секции `[signature]`, `[hints]`, `[examples]`, `[check]`), битый файл пропускается с `profile_invalid`; встроенные профили по образцам из 4.6 (`wirex.txt` — 10.5, `tbank.txt` — 11.4); проверка — тест загрузки каждого файла профиля (и что он целиком в бюджет 500) и битого файла
- [x] 4.8 Отпечаток формата (строка заголовка колонок, словарь, нормализация) и автоопределение (`auto`, `none`, `ambiguous`); `profile` в `POST /imports`, `unknown_profile`; проверка — тесты: CSV за два месяца дают один отпечаток; две совпавшие сигнатуры — `ambiguous`
- [x] 4.9 Профиль в промпте `extract_rows` с бюджетом `AI_PROFILE_MAX_TOKENS`, уменьшение фрагмента на бюджет; `[check]` в проверке (стоп-слова, форматы даты, знак расхода перекрывает `dir`); проверка — тест сборки промпта (длина ≤ контекста) и тест «остаток против суммы» с `FakeLlm` (фрагмент урезается только при нехватке окна `LLM_CONTEXT` — D4; тесты `rows_request_and_response`, `import_profile_in_prompt_and_check`)
- [x] 4.10 Свои форматы: `format_profile`/`format_example` в миграции 1.3, пример из `PATCH rows` при правке суммы, даты или направления, 5 последних, свой формат поверх встроенного; `GET/PATCH/DELETE /api/ai/statement-profiles*`, 403 на встроенный, 404 на чужой; проверка — юнит-тесты; повторная выписка того же формата получает свои примеры
- [x] 4.11 Сравнение на образцах из 4.6 с профилем и без (`wallet_ai_llm_try`): доля `verified`, `check_failed`, `unparsed`; результат — в `design.md` → «Наблюдения после прогона»; проверка — у профиля качество не хуже, чем без него (ТБанк: 14→20 из 20 и 18→24 из 25; Wirex: 0→106 из 106)

## 5. Категории

- [x] 5.1 `statement/pattern.*` — нормализация описания; проверка — тесты на типичные описания (`YANDEX*GO MOSCOW RUS 4021` → `YANDEX*GO`)
- [x] 5.2 Правила `merchant_rule`, затем пачки `classify_rows` по 20 `pattern` (`prompts/classify_rows.txt`, `enum` категорий + `""`), порог `AI_CLASSIFY_MIN_CONFIDENCE`, `suggestion`; при сбое — `ready` без категорий; проверка — тесты с `FakeLlm`

## 6. Планировщик импорта

- [x] 6.1 `ImportRunner`: одно задание на импорт в очереди, круговой обход, пауза при неуспешной пробе LLM, `AI_IMPORT_PAUSE_MAX_MINS`, три таймаута фрагмента → `failed: timeout`, `retry`; проверка — тесты на чередование A1 B1 A2 и на паузу
- [ ] 6.2 Возобновление при старте по `statement_chunk.status`; проверка — интеграционно: `docker compose restart wallet-ai` посреди разбора, импорт доходит до `ready` без повторных фрагментов
- [x] 6.3 `expires_at` = создание + `AI_IMPORT_TTL_DAYS` (90), при `done` — min(сейчас + `AI_IMPORT_DONE_TTL_DAYS` (7), создание + 90); чистка раз в час; `DELETE` с отменой заданий; проверка — тест с подменённым временем: `ready` на 89-й день жив, на 91-й удалён; `done` удалён через 7 дней, `statement_imported` остался

## 7. Проверка и запись

- [x] 7.1 `GET /api/ai/imports` и `GET /api/ai/imports/{id}`: группы, строки с источниками, готовые операции, 404 на чужой id; проверка — юнит-тесты на группировку
- [x] 7.2 Сверка с внесёнными (D7) через `/api/reports/operations` с курсором, ±1 день, «одна операция — одна строка», `statement_imported`, кэш на минуту, `?reconcile=1`, `reconciled: false` при сбое wallet-report; проверка — тесты с фейковым `reports_client`
- [x] 7.3 `PATCH /rows` (категория → правило и соседние строки, сумма и дата снимают `check`, `decision`), `POST /rows` из «не разобрано», `POST /committed` → `statement_imported`, статус `done`; проверка — юнит-тесты

## 8. Наблюдаемость и документация

- [ ] 8.1 Метрики `ai_statement_imports_total{status}`, `ai_statement_rows_total{result}`, `ai_statement_chunk_duration_seconds`; логи без текста и сумм; проверка — `/metrics` после прогона, `grep` логов на отсутствие сумм
- [x] 8.2 `openapi.yaml` (все `/api/ai/imports*`, `/api/ai/statement-profiles*`, `features`), README (раздел «Импорт выписки», переменные окружения); проверка — `/docs` открывается, схемы совпадают с ответами
- [x] 8.3 `org/docs/TODO_statement_import.md` и `TODO_wallet_ai.md` §10 шаг 6 — отступление от §1 (LLM извлекает строки) и выбор варианта «В»; проверка — документы ссылаются на этот change

## 10. Правки по образцу Wirex (2026-10-23, design D4, D11, D12)

- [x] 10.1 `statement/source`: UTF-16LE/BE по BOM и без BOM (нулевые байты через один); проверка — тест на `tests/statements/wirex/statement_eur.csv` как есть, тест BOM LE/BE
- [x] 10.7 Импорт без единой строки с суммой (и без строк по колонкам) — `failed: no_operations`; проверка — тест «текст без операций» (`import_without_operations_fails_fast`: текст и CSV по колонкам; текст не хранится, `retry` — 409)
- [x] 10.2 `statement/verify`: даты `ДД-ММ-ГГГГ`, `ДД/ММ/ГГГГ` и время после даты; «строка с суммой» — два знака после разделителя или дата в начале и число (спека «Строка с суммой»); проверка — табличные тесты, сценарии «Сумма без копеек», «Заголовок даты», «Дата через дефис со временем»
- [x] 10.3 `rows_max_tokens` от объёма фрагмента: `200 + 100 × строк с суммой + символов / 6`, ≤ 2400 (100, а не 40 — замер на ТБанке, design D4); проверка — тест; промпт + ответ ≤ 4096 на фрагменте 1800 символов
- [x] 10.4 `statement/profiles`: секции `[columns]` (разделитель, колонки по имени из заголовка, формат даты) и `[types]`; разбор CSV по колонкам без `extract_rows` — строки сразу в базу, проверка D3 и отпечатки как у строк модели, импорт сразу `classifying`/`ready`; профиль без нужной колонки не применяется; проверка — юнит-тесты разбора, возврата (`+` у расхода → `income`), неизвестного типа, импорта без заданий LLM
- [x] 10.5 `prompts/banks/wirex.txt` (сигнатура — заголовок CSV Wirex, `[columns]`, `[types]`); проверка — тест на `tests/statements/wirex/statement_eur.csv`: 106 строк разобраны, ни одной в «не разобрано», обмены и переводы — `transfer`, возврат Glovo — `income`, остатки не взяты суммами
- [x] 10.6 `pattern_of`: префикс `CARD NNNN :`, город и страна через запятую, `KASA N`, номер магазина в конце (до 3 цифр, отдельно или приклеен); `36.6` остаётся; проверка — сценарии спеки «Покупка Wirex», «Число в названии», прежние тесты pattern (заодно: у слова с приклеенным длинным номером остаётся слово — «Sdek0737» → `SDEK`, раньше выпадало целиком)

## 11. Правки по образцу ТБанка (2026-10-23, design D4, D10)

- [x] 11.1 `[check] transfer_words`: слова в строках-источниках ставят `dir: transfer` сильнее знака и модели; проверка — тест «Перевод себе» с `+` → `transfer`, «Payment» с `−` и `income` модели → `expense`
- [x] 11.2 Строки примеров профиля нумеруются с 37 (следующий пример — дальше), а не с 1; проверка — тест «37| …» и `"lines":[37]`
- [x] 11.3 `prompts/extract_rows.txt` → extract-v3: «lines — число перед «|», а не порядковый номер во фрагменте»; проверка — прогон ТБанка без потерянных фрагментов из-за номеров
- [x] 11.4 `prompts/banks/tbank.txt` (сигнатура `tbank.ru` + `127287` — общая для русской и английской версий; подсказки про две суммы, дату операции, номер карты; 2 обезличенных примера; стоп-слова итогов; `transfer_words`); проверка — тест загрузки, прогон `wallet_ai_llm_try --profile` на обеих выписках
- [x] 11.5a `wallet_ai_llm_try --profile` с `[columns]` разбирает CSV по колонкам без LLM (`by_columns: true` в итоге)
- [x] 11.5 Обезличенные образцы ТБанка в `tests/statements/tbank/` (ФИО, адрес, номера договоров, счетов, карт и телефонов, суммы); проверка — структура строк и колонок как у банка, итоги «Пополнения/Расходы» сходятся (текст после `pdftotext -layout`, суммы × 0.73, пары переводов сохранены; тест `tbank_samples_detect_and_totals`)
- [x] 11.6 Встречные переводы между своими кошельками (ТБанк: карта ↔ накопительный) — решение в design «Open Questions»; отдельный change вместе с записью переводов (вынесено в change `add-statement-transfers`)
- [x] 11.7 Пересчёт `lines` по сдвигу (все номера меньше начала фрагмента, сдвиг одинаковый) — только если 11.2 и 11.3 не убрали ошибку; проверка — тест с `FakeLlm`, отдающим номера от 1 (не понадобилось: в 7 прогонах ТБанка после 11.2/11.3 номера от 1 не встретились — design D4)

## 9. Живой прогон

- [ ] 9.1 Три настоящие выписки (PDF одного банка, CSV другого, вставленный текст) через `curl` с токеном: доля `verified`, `check_failed`, `unparsed` записана в `design.md` → «Наблюдения после прогона»; проверка — суммы `ready` сходятся с итогом выписки за вычетом групп
- [ ] 9.2 Трата текстом во время импорта ждёт не дольше одного фрагмента; проверка — замер `queue_ms`
- [x] 9.3 `openspec validate add-statement-import --strict`; проверка — change валиден
