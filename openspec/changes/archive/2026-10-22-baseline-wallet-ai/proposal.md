# Proposal: базовая спецификация wallet-ai

## Why

`wallet-ai` работает (шаги 0–4 `org/docs/TODO_wallet_ai.md`): статус ИИ,
асинхронные задания и разбор траты текстом, которым пользуется Telegram-бот.
Его поведение описано только в коде, тестах, README и общем плане. Следующее
изменение — импорт выписки (`add-statement-import`) — добавляет новый вид
задания, хранилище и API, поэтому сначала нужно зафиксировать текущее
поведение как базу, не меняя код.

## What Changes

- Восстанавливается спецификация **текущего** поведения по `src/`, `tests/`,
  `prompts/expense.txt`, `openapi.yaml`, `README.md`, `Containerfile`,
  `.env.example`, `scripts/lms-load.sh`.
- Вендоренный wallet-common (`expense/`, `clients/`, `util/`) описывается только
  в той части, которую видит клиент wallet-ai (проверки, resolver, исходы).
- LM Studio, wallet-report, google-auth и гейтвей — только как внешние границы.
- Обоснования из `TODO_wallet_ai.md` переносятся в `design.md`, расхождения
  документов и кода — в «Наблюдения»; требованиями они не становятся.
- Код, конфигурация и поведение **не меняются**. Нереализованное (импорт, чат,
  шаблоны, MCP, eval, автозапуск LM Studio) в базу не входит.

## Capabilities

### New Capabilities

- `ai-access`: проверка access-токена — подпись по JWKS, `iss`/`aud`/`scope`,
  отказ токенам старого формата.
- `ai-status`: `GET /api/ai/status` и фоновая проба LLM.
- `ai-jobs`: асинхронные задания — создание, очередь и воркеры, приоритеты,
  лимиты, long-poll, отмена, TTL, идемпотентность, доступ только владельцу.
- `expense-parsing`: задание `parse_expense` — запрос к LLM, проверки, resolver,
  исходы `draft`/`not_expense`/`rejected` и причины `failed`.
- `service-operations`: служебные ручки, конфигурация из окружения, промпт из
  файла, метрики и логи без пользовательского текста.

### Modified Capabilities

<!-- Нет: в openspec/specs/ пока нет ни одной спецификации. -->

## Impact

- Новые файлы только в `openspec/` (и `.claude/` от `openspec init`).
- Код, `openapi.yaml`, `Containerfile`, CI — без изменений.
- Другие репозитории не затрагиваются.
