# Tasks

## 1. Сборка и `/version`

Сборка и тесты — в Docker, по одному сервису за раз.

- [ ] 1.1 `bash ../wallet-common/scripts/vendor.sh .` (после задачи 1.1 change wallet-gateway `service-versions`); `src/main.cpp` — новая `set_build_info(service, version, build, commit)` и `build` в строке лога при старте. Проверка: юнит-тесты `tests/` в Docker проходят
- [ ] 1.2 `CMakeLists.txt` (`APP_BUILD`, по умолчанию `${APP_VERSION}-dev`), `src/version.hpp.in` (`kBuild`), `Containerfile` (`ARG APP_BUILD`, `-DAPP_BUILD`). Проверка: образ без аргументов → `curl 127.0.0.1:8087/version` в контейнере содержит `"build":"<VERSION>-dev"`
- [ ] 1.3 `src/docs.cpp`: `build` и `started_at` в `/version`; `openapi.yaml` (схема `/version`) и README. Проверка: два `curl /version` подряд — одинаковый `started_at`; `/metrics` содержит `process_start_time_seconds` и `build_info{…build=…}`
- [ ] 1.4 `.github/workflows/release.yml`: `APP_BUILD=${{ needs.meta.outputs.version }}-rc.${{ needs.meta.outputs.rc }}` в `build-args` rc-image; rc-тег. Проверка: выкат в test прошёл smoke, элемент wallet-ai в `GET /api/versions` гейтвея с `build` = тег
