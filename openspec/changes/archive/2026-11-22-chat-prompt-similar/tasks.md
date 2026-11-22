# Tasks

Задача: [wallet-ai#31](https://github.com/WalletTransactions/wallet-ai/issues/31).

## 1. Промпт

- [x] 1.1 Правило про `similar` в `prompts/chat.txt`, `version: chat-v4` (D1–D4); README — версия промпта. Проверка: `chat_prompt_substitutions_and_real_file` ждёт `chat-v4` и слово `similar`; `docker build --target test` зелёный — выполнено 2026-11-22: 172 теста зелёные

## 2. Выкат

- [x] 2.1 PR в main, CI зелёный, VERSION поднята, rc wallet-ai на test. Проверка: строка `ok` в `~/.local/state/wallet-envs/test/history`, в журнале wallet-ai `prompt_version: chat-v4` _2026-11-22: PR #32 влит, `wallet-ai 0.2.6-rc.1` на test (`history`: `ok` 12:29Z), в работающем контейнере `/app/prompts/chat.txt` — `version: chat-v4`; финальный `0.2.6` выкачен на prod._

## 3. Живой прогон на test

- [x] 3.1 «Сколько потрачено на кафе в двадцать шестом году» от имени пользователя общего леджера. Проверка: ответ называет Restaurants и Café (и Bar) с суммами, а не «0 евро»; не помогло с двух попыток — записать как «качество модели» (D7 `chat-prompt-v3`), пункт закрыть _2026-11-22, test, пользователь `web-test-user` (данные заведены через `POST /sync/push`: «Кафе и рестораны» без трат, Restaurants 987.39 за 64, Café 112.44 за 14, Bar 49.80 за 5, EUR, 2026; qwen3-8b как `wallet-parse`): оба прогона (из двух) назвали Restaurants, Café и Bar с суммами и операциями, не «0 евро»; 3 шага, инструменты `ledger_overview`, `spending_by_category`. Замечено вне критерия: сумма «всего похожих» в ответах неверна (1150.63 вместо 1149.63; «0.00 EUR») — модель складывает сама, см. комментарий в wallet-ai#31._
