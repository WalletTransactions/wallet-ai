#include "docs.hpp"

#include <fstream>
#include <sstream>

#include "nlohmann/json.hpp"
#include "util/metrics.hpp"
#include "version.hpp"

namespace docsapi {

namespace {

constexpr const char* kOpenApiPath = "openapi.yaml";
constexpr const char* kVendorDir = "vendor/swagger-ui";

// StandaloneLayout (не BaseLayout) — то же самое, что открыть spec напрямую
// на petstore.swagger.io/docs — с topbar и полем Authorize (нужно, у нас
// bearerAuth/sessionCookie в security, см. components.securitySchemes
// openapi.yaml). tryItOutEnabled — форма сразу готова к вызову, не нужно
// сначала нажимать "Try it out" отдельно на каждом методе.
constexpr const char* kDocsHtml = R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>wallet-ai — API docs</title>
<link rel="stylesheet" href="/vendor/swagger-ui/swagger-ui.css">
<link rel="icon" type="image/png" sizes="32x32" href="/vendor/swagger-ui/favicon-32x32.png">
<link rel="icon" type="image/png" sizes="16x16" href="/vendor/swagger-ui/favicon-16x16.png">
<style>body{margin:0;background:#fafafa}</style>
</head>
<body>
<div id="swagger-ui"></div>
<script src="/vendor/swagger-ui/swagger-ui-bundle.js"></script>
<script src="/vendor/swagger-ui/swagger-ui-standalone-preset.js"></script>
<script>
window.onload = function () {
  window.ui = SwaggerUIBundle({
    url: "/openapi.yaml",
    dom_id: "#swagger-ui",
    presets: [SwaggerUIBundle.presets.apis, SwaggerUIStandalonePreset],
    layout: "StandaloneLayout",
    tryItOutEnabled: true
  });
};
</script>
</body>
</html>
)HTML";

void handle_openapi_yaml(const httplib::Request&, httplib::Response& res) {
    std::ifstream f(kOpenApiPath, std::ios::binary);
    if (!f) {
        res.status = 500;
        res.set_content(
            "openapi.yaml не найден рядом с исполняемым файлом — сборка не докопировала его "
            "(см. CMakeLists.txt/Containerfile)",
            "text/plain; charset=utf-8");
        return;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    // "application/yaml" — не станадртизировано так же жёстко, как
    // application/json, но это ровно то, что показывает Swagger UI/браузер
    // без сюрпризов (не пытается открыть "Save as" как для
    // application/octet-stream).
    res.set_content(buf.str(), "application/yaml; charset=utf-8");
}

void handle_docs(const httplib::Request&, httplib::Response& res) {
    res.set_content(kDocsHtml, "text/html; charset=utf-8");
}

// Liveness для docker compose healthcheck (см. wallet-infra/compose.yaml) —
// нарочно НЕ проверяет LLM: она необязательна, её состояние — в
// /api/ai/status и метрике llm_up, а смешивать "процесс жив" с
// "все зависимости живы" превратило бы обычный blip зависимости в перезапуск
// самого сервиса при restart-политике unless-stopped. Без авторизации — как
// и /openapi.yaml, /docs.
void handle_healthz(const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"status":"ok"})", "application/json; charset=utf-8");
}

// GET /metrics — Prometheus text exposition format (см. util/metrics.hpp,
// infra/monitoring/prometheus.yml). Без авторизации — как и /healthz: это
// внутренняя ручка для скрейпа Prometheus, не часть публичного API сервиса.
void handle_metrics(const httplib::Request&, httplib::Response& res) {
    res.set_content(metrics::render(), "text/plain; version=0.0.4; charset=utf-8");
}

// GET /version — версия ЭТОГО конкретного билда (см. CMakeLists.txt, "Версия
// приложения": version.hpp сгенерирован CMake из VERSION + git-хэша на
// момент configure). Нужна при разборе инцидента/деплое — понять, какой
// именно билд сейчас отвечает, не расшифровывая build_info из /metrics и не
// сравнивая образы вручную (та же тройка полей, что и там — см.
// util/metrics.cpp). Без авторизации — как и /healthz, /metrics: внутренняя
// диагностическая ручка, не часть публичного API.
void handle_version(const httplib::Request&, httplib::Response& res) {
    nlohmann::json j;
    j["service"] = version::kService;
    j["version"] = version::kVersion;
    j["build"] = version::kBuild;
    j["git_commit"] = version::kGitCommit;
    j["started_at"] = metrics::started_at_iso();
    res.set_content(j.dump(), "application/json; charset=utf-8");
}

} // namespace

void register_routes(httplib::Server& svr) {
    svr.Get("/openapi.yaml", handle_openapi_yaml);
    svr.Get("/docs", handle_docs);
    svr.Get("/healthz", handle_healthz);
    svr.Get("/metrics", handle_metrics);
    svr.Get("/version", handle_version);
    // set_mount_point отдаёт статику по префиксу пути — здесь это ТОЛЬКО
    // сам Swagger UI (JS/CSS/favicon), не пересекается ни с одним реальным
    // API-путём сервиса (все они начинаются с /api/ или /sync/).
    svr.set_mount_point("/vendor/swagger-ui", kVendorDir);
}

} // namespace docsapi
