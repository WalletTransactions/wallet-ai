# Tasks

Задача: [wallet-ai#13](https://github.com/WalletTransactions/wallet-ai/issues/13).

## 1. Промпт

- [x] 1.1 Правило в `chat.txt`: имя категории или кошелька, которого нет в кошельке, сопоставлять с именами из ответа инструмента / `list_categories` / `list_wallets` (перевод — не неоднозначность) и вызывать снова без переспроса (D2)
- [x] 1.2 Правило: «без X», «кроме X» → `exclude_categories` без `categories`; «лето против остальных месяцев» → два вызова `spending_by_category_by_month` за весь период (не назван — 24 полных месяца), сравнение `avg_per_month` (D3, D4)
- [x] 1.3 Правило: в `search_operations.text` — несколько вариантов слова (основа, синонимы, связанные товары и услуги, перевод), при неоднозначном совпадении — проверка категории (D5)
- [x] 1.4 `version: chat-v3`; README — версия промпта. Проверка: `ctest` — `chat_prompt_substitutions_and_real_file` ждёт `chat-v3` и опорные слова правил (D6) — выполнено 2026-11-08: `docker build --target test`, 142 теста зелёные

## 2. Выкат

- [x] 2.1 PR в main, CI зелёный, rc wallet-ai на test. Проверка: строка `ok` в `~/.local/state/wallet-envs/test/history`, в журнале wallet-ai — `chat-v3` — выполнено 2026-11-08: PR #14, CI зелёный, `v0.2.0-rc.3` — `ok` в history 20:19:57 UTC, `chat_config` пишет `prompt_version: chat-v3`

## 3. Живые прогоны на test

- [ ] 3.1 «Сколько в среднем в месяц я трачу на алкоголь за два года». Проверка: в журнале wallet-mcp — `spending_by_category_by_month` с `categories: ["Alcohol"]`, ответ называет `avg_per_month` из инструмента — Тест: [wallet-ai#15](https://github.com/WalletTransactions/wallet-ai/issues/15)
- [ ] 3.2 «Насколько июнь–август дороже остальных месяцев по категориям, без ремонта». Проверка: в журнале — `exclude_categories` без `categories`, `months_of_year` [6,7,8] и остальные месяцы за один и тот же период длиннее года, ответ сравнивает `avg_per_month` — Тест: [wallet-ai#15](https://github.com/WalletTransactions/wallet-ai/issues/15)
- [ ] 3.3 «Когда я последний раз стригся?». Проверка: в журнале — `search_operations` с несколькими вариантами в `text`, ответ — 04.03.2026 — Тест: [wallet-ai#15](https://github.com/WalletTransactions/wallet-ai/issues/15)
- [ ] 3.4 Отметки в tasks.md wallet-mcp: `add-category-analytics` 6.2, `search-operations-variants-and-paging` 4.2, `search-default-period` 2.2 (не помогло с двух попыток — «качество модели», D7) — Тест: [wallet-ai#15](https://github.com/WalletTransactions/wallet-ai/issues/15)
