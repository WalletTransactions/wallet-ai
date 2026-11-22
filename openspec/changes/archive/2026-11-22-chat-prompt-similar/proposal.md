# Proposal

Задача: [wallet-ai#31](https://github.com/WalletTransactions/wallet-ai/issues/31).

## Why

wallet-mcp 0.3.7 (change `add-similar-categories-hint`) добавил в строки с
`amount: "0"` поле `similar` — категории той же темы, где расход есть. На
test вопрос «сколько потрачено на кафе в двадцать шестом году» дошёл до
инструмента как `categories: ["Кафе"]`, инструмент ответил `amount: 0.00` и
`similar: Restaurants 987.39 (64), Café 112.44 (14), Bar 49.80 (5)`, а чат
(LM Studio, `wallet-parse`, 8B) ответил «0 евро». Модель читает поле `amount`
и пересказывает его: подсказки в описании инструмента маленькой модели не
хватает, правило должно быть в системном промпте.

## What Changes

- `prompts/chat.txt`: правило про `similar` — при `amount` 0 и непустом
  `similar` не отвечать «0», а вызвать инструмент ещё раз с `categories` из
  `similar` (тот же период) и ответить по этим строкам и их `total`; пустой
  `similar` — расхода по теме нет.
- Версия промпта `chat-v4`, README, тест на реальный файл промпта.

## Capabilities

### Modified Capabilities

- `ai-chat`: новое требование к системному промпту — поле `similar`.

## Impact

- Код: `prompts/chat.txt`, `tests/test_chat.cpp`, `README.md`, `VERSION`.
- Другие сервисы: правок нет (wallet-mcp уже отдаёт `similar`).
- Совместимость: промпт читается при старте, нужен только рестарт. Сервис,
  который не вернул `similar`, ведёт себя как раньше.
