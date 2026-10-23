// Маршруты /api/ai/imports* и /api/ai/statement-profiles* (imports.hpp).
// Без базы ai или без промптов импорта — 503 {"error": "unavailable"}.
#pragma once

#include "app_state.hpp"
#include "httplib.h"

namespace imports {

void register_routes(httplib::Server& svr, AppState& state);

} // namespace imports
