# Tasks

Отложено до появления ключа Anthropic API.

## 1. Конфиг test

- [ ] 1.1 Ключ Anthropic API (console.anthropic.com) с лимитом расходов. Задать в wallet-infra `wallet-ai.env` test: `ANTHROPIC_API_KEY`, `ANTHROPIC_ALLOWED_USERS=<user_id тестового пользователя>`. Проверка: в журнале wallet-ai `chat_config` — `"claude": true`; `GET /api/ai/settings` — `claude` `available: true`

## 2. Живые проверки на test

- [ ] 2.1 (из wallet-ai `add-chat-agent` 6.4) Задание `chat` «сколько я потратил на няню и еду в сентябре?» с `provider: claude` (тестовый пользователь в `ANTHROPIC_ALLOWED_USERS`, согласие дано). Проверка: сумма совпадает с `/api/reports/expense-structure` за сентябрь (3503.22 EUR на 2026-11-04), задание выполнено облачной полосой при занятом локальном воркере, `ai_chat_tokens_total{provider="claude"}` вырос
- [ ] 2.2 (из wallet-telegram `add-ai-chat` 6.2) В тестовом боте `/ai` → «Claude» → «Согласен» → тот же вопрос. Проверка: подпись «— Claude», сумма та же; «Отозвать согласие» возвращает LM Studio

## 3. Prod

- [ ] 3.1 Решение по модели и лимитам (`ANTHROPIC_MODEL`, `ANTHROPIC_MAX_TOKENS`, `AI_CLOUD_MAX_CONCURRENT`, `ANTHROPIC_ALLOWED_USERS`), ключ prod — в wallet-infra `wallet-ai.env` prod. Проверка: те же 2.1–2.2 на prod одним пользователем
