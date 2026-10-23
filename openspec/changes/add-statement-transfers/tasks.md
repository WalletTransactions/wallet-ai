# Tasks

Опирается на код `add-statement-import` (`3427c66`). Сборка и тесты — в
Docker, по одному сервису за раз (`docker compose ... --env-file
wallet-infra/infra/db/.env`). Юнит-тесты — `tests/` с `MemRepo`, `FakeLlm`,
`FakeReports`.

## 1. Данные

- [ ] 1.1 `migrations/000002_statement_rules.{up,down}.sql`: `statement_rule` (T2) с переносом `merchant_rule` и её удалением; в `statement_row` — `flow`, `counter_wallet_uuid`, `counter_source`, `source_type`, `ref`, `rule_ignored`; проверка — `bash infra/db/migrate.sh ai up`, `down`, `up` на базе с правилами категорий: правила сохранились
- [ ] 1.2 `stmt::Repo`/`pg_repo`/`MemRepo`: `rules(user_id)` → список `Rule{id, key_kind, key, action, target_uuid, updated_at}`, `upsert_rule(user, key_kind, key, action, target)`, `delete_rule(id, user)`; новые поля `RowRec` в `kInsertRow`/`row_of`/`save_rows`; проверка — юнит-тесты `MemRepo` и сборка `pg_repo`

## 2. Направление и ключи строки

- [ ] 2.1 `verify_row` отдаёт знак найденной суммы; у `dir: transfer` — `flow` `out`/`in`/пусто (T1); `parse_by_columns` — знак колонки и `source_type` из колонки типа; проверка — тесты: «+… Перевод себе» → `in`, Wirex `Exchange -66.94` → `out`, строка без знака → пусто
- [ ] 2.2 `ref_of(lines | description)`: самое длинное число из 6+ цифр подряд, последние 4 цифры (T3); для строк по колонкам — только описание; проверка — тесты: договор, счёт из 20 цифр, телефон, сумма «120 000.00» не даёт ref, «3841» не даёт ref
- [ ] 2.3 Заполнение `flow`, `ref`, `source_type` в `import_runner::extract` и при разборе по колонкам в `Service::create`; пересчёт пустых `ref`/`source_type` из `source_text` при открытии старого импорта; проверка — тест импорта ТБанк-подобного текста: у переводов `flow` и `ref`

## 3. Правила

- [ ] 3.1 Функция применения правил (порядок `type` → `ref` → `pattern`, T4; ручной выбор не перекрывается; правило на удалённую или архивную категорию или кошелёк пропускается) — вместо поиска `merchant_rule` в `finish_extraction` и в правках; проверка — тесты сценариев «Договор карты», «Тот же текст, другой договор», «Снятие в банкомате», прежние тесты правил категорий
- [ ] 3.2 Обучение: категория → правило `category` по `pattern` (как раньше); `counter_wallet_uuid` → правило `transfer` по `ref`, иначе `type`, иначе `pattern`; `decision: ignore` → правило `ignore` по `type`, иначе `pattern`; применение к остальным строкам импорта; проверка — тесты «Один выбор на весь импорт», «Игнорировать проценты», «Замена правила»
- [ ] 3.3 `GET /api/ai/statement-rules`, `DELETE /api/ai/statement-rules/{id}` (404 на чужое); роуты в `main.cpp` под `ai:use`; проверка — юнит-тесты сервиса, `curl` через гейтвей

## 4. Группы, правки, готовые операции

- [ ] 4.1 `group_of`: `ignored`, `income`, `transfer` + `transfer_reason` (`no_wallet`, `no_flow`, `currency_differs`), переводы по веткам расхода без `needs_category`; `closed()` по спеке «Импорт закрыт без переводов»; проверка — тест «Сводка выписки накопительного счёта», «Кошелёк в другой валюте», «Переводы без кошелька» → `done`
- [ ] 4.2 `PATCH rows`: `counter_wallet_uuid` (`unknown_wallet`, `same_wallet`, `not_transfer`), `flow` (`bad_flow`), `decision: ignore`, `include` у `ignored`; проверка — тесты ошибок и сценария «Вернуть одну строку»
- [ ] 4.3 Готовая операция перевода (`out`: импорт → второй, `in`: второй → импорт) в `view`; поля строки `flow`, `counter_wallet_uuid`, `counter_source`, `transfer_reason`; проверка — тест «Перевод с накопительного на карту»

## 5. Сверка

- [ ] 5.1 `fetch_ops` берёт и `kind: transfer` с кошельком импорта в `from_uuid`/`to_uuid`; `reconcile` сравнивает перевод только с переводами той же пары и направления, ±1 день, одна операция — одна строка; проверка — тесты «Вторая выписка той же пары счетов», «Перевод в другую сторону не совпадает», `FakeReports` с переводами

## 6. Документация и прогон

- [ ] 6.1 `openapi.yaml`: поля строки, решения, ошибки `PATCH rows`, группы, `/api/ai/statement-rules*`; README — раздел «Импорт выписки»: переводы, правила, игнор; проверка — `/docs` открывается, схемы совпадают с ответами тестов
- [ ] 6.2 Живой прогон на двух выписках ТБанка (`statementExamples/`, два кошелька в RUB): импорт накопительного → выбор кошельков у переводов → запись → импорт карты; проверка — у переводов карты, записанных из первого импорта, `match: exists`, к записи — только расходы карты и непарные переводы; итог — в `design.md` («Наблюдения после прогона»)
- [ ] 6.3 `openspec validate add-statement-transfers --strict`; проверка — change валиден

## Workflow follow-up

- Перед архивацией: сверить с архивированными спеками `statement-review`
  («Группы строк», «Правки строк», «Готовые операции») и
  `statement-categorization` («Правила пользователя», «Обучение на правках»)
  из `add-statement-import`. Если тот архивирован раньше, перенести эти правки
  как MODIFIED.
- wallet-web, change `add-ai-statement-import`: группы `income`/`transfer`/
  `ignored`, выбор второго кошелька, «Игнорировать всегда», страница правил.
