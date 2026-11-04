# Proposal

## Why

Провайдер `claude` для задания `chat` написан и покрыт юнит-тестами в
`add-chat-agent`: адаптер Anthropic Messages API, облачная полоса, согласие,
`ANTHROPIC_ALLOWED_USERS`. В боте есть выбор кнопкой и экран согласия
(wallet-telegram `add-ai-chat`). Но живьём его не проверить: ключа Anthropic
API (console.anthropic.com, платный доступ отдельно от подписки claude.ai)
нет. На test `chat_config` пишет `"claude": false`, `/api/ai/settings` —
`claude` `available: false`, `reason: not_configured`.

Живые проверки Claude вынесены сюда, чтобы закрыть `add-chat-agent` и
`add-ai-chat` по LM Studio. Change отложенный: браться, когда появится ключ.

## What Changes

- Код не меняется. Включение — конфигурацией wallet-ai в wallet-infra:
  `ANTHROPIC_API_KEY`, `ANTHROPIC_ALLOWED_USERS` (сначала один тестовый
  `user_id`), при необходимости `ANTHROPIC_MODEL` (по умолчанию
  `claude-opus-5-5`; дешевле — `claude-haiku-5-5`).
- Живые проверки, перенесённые из других changes:
  - wallet-ai `add-chat-agent` 6.4 — задание `chat` с `provider: claude`;
  - wallet-telegram `add-ai-chat` 6.2 — `/ai` → «Claude» → согласие → вопрос.
- Перед prod — решение по стоимости: модель, `ANTHROPIC_MAX_TOKENS`,
  `AI_CLOUD_MAX_CONCURRENT`, круг пользователей.

## Capabilities

### New Capabilities

Нет.

### Modified Capabilities

Нет — требования к провайдеру `claude` уже в спеках `ai-providers` и
`ai-chat` (`add-chat-agent`).

## Impact

- **Конфиг:** wallet-infra, `wallet-ai.env` test, затем prod.
- **Деньги:** токены Anthropic API по тарифу; метрика
  `ai_chat_tokens_total{provider="claude"}`.
- **Приватность:** вопросы и нужные для ответа данные кошелька уходят в
  Anthropic — только после согласия пользователя в боте.

Дельт спек нет, поэтому `openspec validate` этот change не принимает;
архивировать — `openspec archive enable-claude-provider --skip-specs`.
