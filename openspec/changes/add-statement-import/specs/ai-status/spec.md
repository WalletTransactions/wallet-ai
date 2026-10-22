# Spec Delta

## ADDED Requirements

### Requirement: Функция statement_import
`GET /api/ai/status` SHALL включать `statement_import` в `features`, когда
LLM доступна и база `ai` отвечает. Без базы импорт не объявляется, а
`POST /api/ai/imports` отвечает 503 `{error: "unavailable"}`; `parse_expense`
от базы не зависит.

#### Scenario: Всё работает
- **WHEN** проба LLM успешна и база `ai` доступна
- **THEN** `features` — `["parse_expense", "statement_import"]`

#### Scenario: База недоступна
- **WHEN** Postgres базы `ai` не отвечает
- **THEN** `features` — `["parse_expense"]`, трата текстом работает, импорт — 503

### Requirement: Импорт видит выключение LLM
Пока LLM недоступна (`enabled: false`), уже созданные импорты SHALL оставаться в
своём статусе и продолжать разбор, когда проба снова успешна, а не падать
сразу.

#### Scenario: LM Studio перезапустили
- **WHEN** во время разбора проба вернула `unreachable`, а через 2 минуты снова успешна
- **THEN** импорт продолжает разбор с того фрагмента, на котором остановился
