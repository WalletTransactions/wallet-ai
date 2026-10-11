// Мини-аналог dotenv: читает KEY=VALUE построчно и кладёт в окружение
// процесса. Переменные, уже выставленные в реальном окружении, не
// перезаписываются — как и в привычных dotenv-библиотеках.
#pragma once

#include <string>

namespace dotenv {

void load(const std::string& path = ".env");

} // namespace dotenv
