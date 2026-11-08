# Proposal

## Why

Задача: [wallet-ai#13](https://github.com/WalletTransactions/wallet-ai/issues/13).

Промпт задания `chat` (`prompts/chat.txt`, `chat-v2`) не доводит модель до
верных вызовов wallet-mcp. Инструменты отвечают верно, ошибается модель. Три
живые проверки на test 2026-11-06 не прошли (wallet-mcp 0.3.0-rc.2/rc.3,
веб-чат, LM Studio `wallet-parse`):

1. «Сколько в среднем в месяц я трачу на алкоголь за два года» — модель
   выбрала верный инструмент и период, но передала `Алкоголь` вместо
   `Alcohol` и на ответ «не найдено. Есть: …» переспросила пользователя
   (wallet-mcp `add-category-analytics` 6.2).
2. «Насколько июнь–август дороже остальных месяцев по категориям, без
   ремонта» — `months_of_year=[6,7,8]` только за 2026 год, без
   `exclude_categories`, без второго вызова и без сравнения; в первой попытке
   `categories` и `exclude_categories` пришли вместе (тот же пункт 6.2).
3. «Когда я последний раз стригся?» — один вариант `text=["стриж"]`, ответ —
   покупка «Машинка для стрижки волос» (Electronics, 07.09.2026) вместо
   стрижки 04.03.2026 (wallet-mcp `search-operations-variants-and-paging`
   4.2, `search-default-period` 2.2).

## What Changes

- `prompts/chat.txt` → `chat-v3`, три новых правила:
  - имя категории или кошелька, которого нет в кошельке, сопоставлять с
    именами из ответа инструмента, `list_categories` или `list_wallets`
    (перевод — не неоднозначность) и сразу вызывать инструмент снова;
  - «без X», «кроме X» → `exclude_categories` без `categories` в том же
    вызове; «лето против остальных месяцев» → два вызова
    `spending_by_category_by_month` за весь период, сравнение по
    `avg_per_month`;
  - в `search_operations.text` — несколько вариантов слова (основа,
    синонимы, связанные товары и услуги, перевод), при неоднозначном
    совпадении — проверка категории операции.
- Юнит-тест на настоящий `chat.txt`: версия `chat-v3` и опорные слова правил.
- README: версия промпта.
- Выкат rc wallet-ai на test, повтор трёх прогонов, отметки в tasks.md
  wallet-mcp (`add-category-analytics` 6.2, `search-operations-variants-and-paging`
  4.2, `search-default-period` 2.2).

Код сервиса не меняется: промпт читается при старте.

## Capabilities

### New Capabilities

Нет.

### Modified Capabilities

- `ai-chat` — требование к правилам system-промпта задания `chat`.

## Impact

- wallet-ai: `prompts/chat.txt`, `tests/test_chat.cpp`, `README.md`.
- wallet-mcp: только `openspec/changes/*/tasks.md` (отметки живых прогонов).
- GPU: промпт длиннее на ~250 токенов на каждый вызов модели в цикле
  (до 6 вызовов на вопрос) — в пределах контекста 8192 (`LLM_CONTEXT`).
