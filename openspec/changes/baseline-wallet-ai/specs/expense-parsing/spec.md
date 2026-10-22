# Spec Delta

## Purpose

Задание `parse_expense` превращает фразу пользователя о трате («вчера такси 450
с тинькоффа») в проверенный черновик: сумму, дату, валюту, кошелёк и категорию
из справочника пользователя. Модель только извлекает поля; проверки и выбор
счетов делает код. Ничего не записывается.

## ADDED Requirements

### Requirement: Вход задания
`input` SHALL содержать `text` (непустой, не длиннее 500 байт) и `now`
(`date` `YYYY-MM-DD`, `time` `HH:MM:SS` — время пользователя); необязательно
`default_wallet_uuid` (строка). Нарушение — 400 с текстом, какое поле не так.
Задание интерактивное.

#### Scenario: Нет now
- **WHEN** `input` без `now`
- **THEN** ответ 400 «input.now обязателен: {date: YYYY-MM-DD, time: HH:MM:SS} в поясе пользователя»

### Requirement: Справочник счетов при создании
При создании задания сервис SHALL запросить `GET /api/reports/accounts` в
wallet-report токеном пользователя и сохранить ответ во входе задания. 401/403
от wallet-report — ответ 401 «wallet-report не принял токен»; другая ошибка — 502
«wallet-report недоступен»; задание не создаётся.

#### Scenario: Токен истёк к запуску
- **WHEN** задание создано короткоживущим токеном бота и ждало в очереди дольше его срока
- **THEN** воркер всё равно разбирает трату: справочник уже лежит в задании

### Requirement: Запрос к LLM
Воркер SHALL делать один `POST {LLM_URL}/chat/completions`: `temperature` 0,
`response_format` — strict `json_schema`, где `category` и `wallet` — `enum` из
имён пользователя плюс `""`; system — промпт `expense.txt` с подстановками; три
примера из имён пользователя; текст пользователя — только в последнем
user-сообщении.

#### Scenario: Чужое имя невозможно
- **WHEN** у пользователя категории «Кафе» и «Такси»
- **THEN** в схеме `category.enum` — `["Кафе", "Такси", ""]`, и модель не может вернуть другое имя

### Requirement: Параметры вызова
Вызов SHALL передавать `max_tokens` из `LLM_MAX_TOKENS_PARSE_EXPENSE` (512) и
`reasoning_effort` из `LLM_REASONING_PARSE_EXPENSE` (`none`; `default` — поле не
передаётся). Дедлайн на весь HTTP-запрос — `AI_JOB_TIMEOUT_SECS_PARSE_EXPENSE`
(60 с), connect — 2 с. Ответ `<think>…</think>` перед JSON вырезается.

#### Scenario: Долгая генерация
- **WHEN** LLM не ответила за 60 с
- **THEN** задание завершается `failed` с `failure: timeout`

### Requirement: Отказы LLM
Ответ LLM SHALL переводиться в `failed` с `failure`: `timeout` — истёк дедлайн или
ошибка чтения; `bad_response` — 400, не JSON, нет `choices`, `finish_reason:
length`, поле вне схемы; `unavailable` — сеть, 5xx, 401/403, 404. Исключение в
воркере — `failed` с `internal`.

#### Scenario: Оборванный JSON
- **WHEN** ответ модели пришёл с `finish_reason: "length"`
- **THEN** задание `failed`, `failure: bad_response`, в лог — `llm_detail: finish_reason_length`

### Requirement: Не трата
Если модель вернула пустую `amount`, задание SHALL завершиться `done` с
`result: {outcome: "not_expense", prompt_version}`.

#### Scenario: Приветствие
- **WHEN** текст «привет, как дела»
- **THEN** результат `outcome: not_expense`

### Requirement: Проверки суммы
Сумма модели SHALL разбираться тем же разбором, что строгий формат бота («1200,50»
→ «1200.5»). Не число — `rejected` с `bad_amount`, ноль — `zero_amount`. Сумма
должна встречаться в исходном тексте как число, иначе `rejected` с
`amount_not_in_text`.

#### Scenario: Модель сложила суммы
- **WHEN** текст «кофе 200 и булка 150», а модель вернула `350`
- **THEN** результат `outcome: rejected`, `reasons: ["amount_not_in_text"]`

### Requirement: Проверки даты
Пустая дата SHALL означать сегодня (`now.date`). Некорректная — `rejected` с
`bad_date`; позже `now.date` — `future_date`; старше 366 дней — `too_old`. Время
черновика берётся из `now`.

#### Scenario: Будущая дата
- **WHEN** модель вернула завтрашнюю дату
- **THEN** результат `rejected`, `reasons: ["future_date"]`

### Requirement: Валюта и комментарий
Валюта модели SHALL сохраняться как `currency_hint`, только если она известна
справочнику пользователя, иначе отбрасывается с пометкой `currency_dropped`.
Комментарий обрезается до 500 байт по границе UTF-8 с пометкой
`comment_truncated`. Пометки идут в `notes` черновика.

#### Scenario: Неизвестная валюта
- **WHEN** модель вернула `currency: "XYZ"`
- **THEN** черновик без `currency_hint`, в `notes` — `currency_dropped`

### Requirement: Черновик и resolver
Прошедший проверки результат SHALL завершаться `outcome: draft` с `expense`,
`resolution` (resolver wallet-common по справочнику и `default_wallet_uuid`),
`confidence`, `prompt_version` и `notes`. Resolver решает `found`/`ask` для
категории и `found`/`ask`/`reject` для кошелька, с кандидатами и причиной `why`.

#### Scenario: Кошелёк по умолчанию
- **WHEN** в тексте кошелёк не назван, а `default_wallet_uuid` — живой кошелёк
- **THEN** `resolution.wallet.kind: found` с этим кошельком

#### Scenario: Валюта не совпала с кошельком
- **WHEN** назван кошелёк в RUB, а в тексте евро
- **THEN** `resolution.wallet.kind: ask`, `why: currency_mismatch`, в `force` — названный кошелёк
