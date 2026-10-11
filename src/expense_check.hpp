// Проверка того, что извлекла модель, и приведение к черновику — «Шаг 9
// подробно», 9c.5, org/docs/TODO_telegram_bot.md. Тот же тип, что у строгого
// разбора (parser::ParsedExpense из wallet-common/expense), — дальше черновик
// идёт тем же путём: resolver → карточка → подтверждение пользователем.
//
// Модели здесь не верят: сумма должна дословно встречаться в тексте (иначе
// модель сложила «200 и 450» или выдумала число), дата — не в будущем и не
// старше года, валюта — известная. Чистая функция, без сети.
#pragma once

#include <string>
#include <vector>

#include "expense/strict.hpp"
#include "llm_client.hpp"

namespace expense_check {

// Дальше в прошлое — скорее ошибка модели, чем трата годичной давности.
inline constexpr int kMaxAgeDays = 366;
// Комментарий операции — не роман: длинное описание обрезается.
inline constexpr std::size_t kMaxCommentBytes = 500;

struct Result {
    bool ok = false;
    parser::ParsedExpense expense; // при ok
    // При !ok — что не прошло (для лога и метрики, без текста и сумм):
    // bad_amount, zero_amount, amount_not_in_text, bad_date, future_date,
    // too_old. При ok — что поправлено: currency_dropped, comment_truncated.
    std::vector<std::string> notes;
};

Result check(const llm::Extraction& e, const std::string& text, const parser::Names& names, const parser::Now& now);

} // namespace expense_check
