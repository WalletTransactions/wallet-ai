# Spec Delta

## MODIFIED Requirements

### Requirement: Служебные ручки
Сервис SHALL отдавать без токена `GET /healthz` (`{"status":"ok"}`),
`GET /metrics` (Prometheus, в том числе `build_info` с меткой `build` и
`process_start_time_seconds`), `GET /version`, `GET /openapi.yaml` и
`GET /docs` (Swagger UI). `/version` содержит сервис, версию из `VERSION`,
`build` (тег образа из `APP_BUILD`, без него `<VERSION>-dev`), git-коммит и
`started_at` (время запуска процесса, ISO-8601 UTC). `/healthz` не зависит от
состояния LLM.

#### Scenario: LLM недоступна
- **WHEN** LM Studio остановлен
- **THEN** `/healthz` отвечает 200, а недоступность видна в `/api/ai/status` и `llm_up`

#### Scenario: Версия rc-сборки
- **WHEN** образ собран Release-workflow по тегу `v0.1.1-rc.3`
- **THEN** `/version` отдаёт `"version":"0.1.1"`, `"build":"0.1.1-rc.3"` и `started_at` запуска контейнера
