# Proposal

## Why

`version` в `/version` берётся из файла `VERSION`, поэтому все rc одной версии
выглядят одинаково, а момент запуска сервиса снаружи не виден. Гейтвей собирает
`/version` всех сервисов в сводку `GET /api/versions`, которую показывает бейдж
версии в вебе (change `service-versions` в wallet-gateway и wallet-web). Общие
решения — `wallet-gateway/openspec/changes/service-versions/design.md`.

## What Changes

- `GET /version` получает поля `build` (тег образа из `--build-arg APP_BUILD`,
  без него — `<VERSION>-dev`) и `started_at` (время запуска процесса, ISO-8601 UTC).
  `service`, `version`, `git_commit` не меняются.
- `/metrics`: метка `build` в `build_info` и `process_start_time_seconds`.
- Release-workflow передаёт `APP_BUILD` = тег rc.

## Capabilities

### New Capabilities
- нет

### Modified Capabilities
- `service-operations`: «Служебные ручки» — состав ответа `/version`.

## Impact

`CMakeLists.txt`, `src/version.hpp.in`, `Containerfile`, `src/docs.cpp`,
`src/main.cpp` (`set_build_info`), `third_party/wallet-common/` (vendor.sh),
`.github/workflows/release.yml`, `openapi.yaml`, `README.md`. LLM не участвует,
нагрузки на GPU нет. Других правок, кроме описанных в change wallet-gateway
(wallet-common, wallet-infra), не нужно.
