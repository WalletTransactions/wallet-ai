# Tasks

Изменение не меняет код: задачи — только сверка спецификаций с кодом и тестами.
Найденное несоответствие исправляется в спецификации, если неточна она, или
записывается в «Наблюдения» `design.md`. Код не правится, новые тесты не пишутся:
пробелы покрытия фиксируются для отдельного change.

## 1. Исходное состояние

- [ ] 1.1 Убедиться, что сверка идёт на коммите из `design.md` (`a1f098f`) или зафиксировать новый коммит в Context; проверка — `git log -1` совпадает с записанным
- [ ] 1.2 Прогнать юнит-тесты в Docker (`docker build --target test -f Containerfile .`); проверка — все тесты зелёные, число записано в Context `design.md`

## 2. Сверка по capability

Для каждой спецификации: каждый сценарий сопоставить с тестом (`tests/*.cpp`) или
местом в коде; сценарии без теста перечислить в разделе «Покрытие» `design.md`.

- [ ] 2.1 `ai-access` — `access.cpp`, `session.cpp`, `util/jwt*`; проверка — у каждого сценария ссылка на тест или пометка «нет теста»
- [ ] 2.2 `ai-status` — `llm_probe.cpp`, `ai.cpp::status_json`, `main.cpp::set_llm_status`, `test_llm_probe.cpp`, `test_ai.cpp`; проверка — как в 2.1
- [ ] 2.3 `ai-jobs` — `jobs.cpp`, `ai.cpp` (`handle_create/get/cancel`, `run_worker`), `test_jobs.cpp`; проверка — как в 2.1, порядок проверок лимитов совпадает с `Store::create`
- [ ] 2.4 `expense-parsing` — `parse_expense.cpp`, `expense_check.cpp`, `llm_client.cpp`, `prompts/expense.txt`, wallet-common `expense/resolver`, `test_parse_expense.cpp`, `test_expense_check.cpp`, `test_llm_client.cpp`; проверка — как в 2.1, тексты ошибок 400 совпадают дословно
- [ ] 2.5 `service-operations` — `main.cpp`, `docs.cpp`, README «Конфиг» и «Метрики», `.env.example`; проверка — список переменных и метрик совпадает с кодом
- [ ] 2.6 Ручной прогон через гейтвей с живой LM Studio: `status`, задание `parse_expense` (draft, not_expense, rejected), отмена, 404 чужого задания; проверка — ответы совпадают со сценариями

## 3. Итоговая проверка

- [ ] 3.1 `openspec validate baseline-wallet-ai --strict`; проверка — change валиден
- [ ] 3.2 Перечитать «Наблюдения» `design.md` после сверки; проверка — у каждого Р-пункта есть файл:строка

## Workflow follow-up

- Заархивировать change (`/opsx:archive`) до применения `add-statement-import`: спецификации переедут в `openspec/specs/`, и дельта импорта ляжет поверх них.
- Р2 и Р4 (README и `.env.example`) — мелкий change документации; Р3, Р5–Р7 — учесть в `add-statement-import`, который добавляет второй вид задания и фоновую очередь.
