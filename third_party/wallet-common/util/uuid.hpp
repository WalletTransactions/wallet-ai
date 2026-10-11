// Минимальный UUID v4 без внешней библиотеки. UUID здесь — непрозрачный
// уникальный идентификатор (для сравнения и передачи в JSON/URL), поэтому
// достаточно хранить/сравнивать его как обычную hex-строку, не заводя
// отдельный тип с арифметикой.
#pragma once

#include <string>

namespace uuidgen {

std::string v4();

// UUID v5 (RFC 4122, SHA-1 от namespace + name): одинаковый вход — всегда
// одинаковый uuid. Нужен для идемпотентности: например, бот строит так
// operation.uuid и batch_id из (tg_user_id, update_id) — повторная обработка
// апдейта после падения даёт тот же батч, и /sync/push отвечает сохранённым
// результатом, а не дублем (org/docs/TODO_telegram_bot.md, §3.3).
// namespace_uuid — в формате 8-4-4-4-12; невалидный — std::invalid_argument.
std::string v5(const std::string& namespace_uuid, const std::string& name);

// Лёгкая проверка формата (8-4-4-4-12 hex) — используется для параметров
// пути, прежде чем использовать их как ключ карты.
bool looks_valid(const std::string& s);

} // namespace uuidgen
