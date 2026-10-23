// Нормализация описания операции в pattern продавца (statement-categorization)
// и отпечаток строки (statement-extraction, D8).
#pragma once

#include <string>

namespace stmt {

// «Yandex*Go  MOSCOW RUS 4021» → «YANDEX*GO»: верхний регистр, без номеров
// карт и терминалов (4+ цифр подряд), без города и страны в конце, пробелы
// схлопнуты.
std::string pattern_of(const std::string& description);

// sha256(wallet_uuid|date|amount|dir|pattern|k), hex; k — номер среди
// одинаковых в импорте (две покупки кофе за день — разные строки).
std::string fingerprint(const std::string& wallet_uuid, const std::string& date, const std::string& amount,
                        const std::string& dir, const std::string& pattern, int k);

// UUID v5 от отпечатка в пространстве имён импорта выписок: повторная
// загрузка той же выписки даёт тот же uuid операции.
std::string op_uuid(const std::string& fingerprint);

} // namespace stmt
