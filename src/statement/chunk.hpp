// Нарезка текста выписки на фрагменты под контекст модели
// (statement-extraction, «Фрагменты под контекст модели»).
#pragma once

#include <string>
#include <vector>

namespace stmt {

struct ChunkPlan {
    int first_line = 1; // номера строк текста с 1, включительно
    int last_line = 0;
    std::string date_context; // последний заголовок даты до фрагмента, как в тексте
};

// Строка начинается с даты («12.09», «12.09.2026», «2026-09-12», «12 сентября»).
bool starts_with_date(const std::string& line);

// Фрагменты не длиннее max_chars символов; пустые строки и строки без цифр в
// длину не входят. Граница — перед строкой с датой в начале или перед пустой
// строкой, иначе перед строкой, на которой кончился лимит, но никогда не внутри
// строки. Фрагменты без денежных сумм (шапка, подвал) не возвращаются.
std::vector<ChunkPlan> plan_chunks(const std::vector<std::string>& lines, std::size_t max_chars);

// «17| текст строки» для first..last — вход модели.
std::string numbered_text(const std::vector<std::string>& lines, int first, int last);

// Строк с денежной суммой во фрагменте — от них считается max_tokens.
int money_lines(const std::vector<std::string>& lines, int first, int last);

} // namespace stmt
