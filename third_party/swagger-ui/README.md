# swagger-ui-dist (вендорено)

Версия **5.32.15**, взято из `https://registry.npmjs.org/swagger-ui-dist/-/swagger-ui-dist-5.32.15.tgz`
(sha1 пакета `0bbaa62695104dbf2db06ad9bbbaaa127d830614` — совпадает с
`dist.shasum` в метаданных npm на момент загрузки). Из пакета (11.7 МБ,
32 файла — source maps, ES-модули, README и т.п.) взяты только 6 файлов,
реально нужных статической странице `/docs`:

- `swagger-ui-bundle.js`, `swagger-ui-standalone-preset.js`, `swagger-ui.css`
  — сам UI;
- `favicon-16x16.png`, `favicon-32x32.png` — иконка вкладки;
- `LICENSE` — Apache-2.0, обязательна к сохранению при вендоринге.

Вендорено (не CDN-ссылка в HTML), как `../httplib.h`/`../nlohmann/json.hpp`
рядом — сервис должен отдавать документацию и без доступа наружу (см.
`org/docs/TODO_mobile_sync.md`, "LAN-only MVP").

**Обновление версии:** скачать новый `swagger-ui-dist-X.Y.Z.tgz`, сверить
sha1 с `dist.shasum` из `GET https://registry.npmjs.org/swagger-ui-dist/X.Y.Z`,
распаковать и заменить те же 6 файлов здесь (и синхронно — в остальных трёх
копиях, см. ниже). Сам UI ничего не знает о версии API — он просто рендерит
`../../openapi.yaml`, который отдаёт сервис (`GET /openapi.yaml`).

**Почему 4 копии, а не одна.** В проекте так же вендорены `httplib.h`/
`nlohmann/json.hpp` — по одной копии в каждом сервисе, без общего
`third_party` на всех (см. соответствующие папки `wallet-api`, `google-auth`,
`wallet-gateway`) — эта копия следует тому же приёму. У `wallet-web`
(nginx, не C++) копия лежит не в `third_party/`, а прямо в `site/vendor/swagger-ui/`
— это и есть то, что реально отдаёт nginx.
