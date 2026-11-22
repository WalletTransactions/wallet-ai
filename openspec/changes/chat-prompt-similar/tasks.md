# Tasks

Задача: [wallet-ai#31](https://github.com/WalletTransactions/wallet-ai/issues/31).

## 1. Промпт

- [x] 1.1 Правило про `similar` в `prompts/chat.txt`, `version: chat-v4` (D1–D4); README — версия промпта. Проверка: `chat_prompt_substitutions_and_real_file` ждёт `chat-v4` и слово `similar`; `docker build --target test` зелёный — выполнено 2026-11-22: 172 теста зелёные

## 2. Выкат

- [ ] 2.1 PR в main, CI зелёный, VERSION поднята, rc wallet-ai на test. Проверка: строка `ok` в `~/.local/state/wallet-envs/test/history`, в журнале wallet-ai `prompt_version: chat-v4`

## 3. Живой прогон на test

- [ ] 3.1 «Сколько потрачено на кафе в двадцать шестом году» от имени пользователя общего леджера. Проверка: ответ называет Restaurants и Café (и Bar) с суммами, а не «0 евро»; не помогло с двух попыток — записать как «качество модели» (D7 `chat-prompt-v3`), пункт закрыть
