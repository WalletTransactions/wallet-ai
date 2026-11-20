## 1. Журнал событий задания

- [x] 1.1 В `jobs.hpp/cpp`: структура `Event` (id, type, data), журнал в `Store` (отдельно от `Job`, чтобы снимки не копировали его), `append_event`, `read_events` с ожиданием на `condition_variable`, чтение с позиции; удаление по TTL вместе с заданием. `done`/`error` ставят `complete()` и `cancel()` атомарно со статусом
- [x] 1.2 Слияние соседних `delta` (только пока читатель не видел последнее событие) и потолок `AI_STREAM_MAX_EVENTS` (текст сверх потолка копится в хвосте и выходит перед `done`/`error`)
- [x] 1.3 Юнит-тесты журнала: порядок id, чтение с `Last-Event-ID`, ожидание, TTL, потолок

## 2. Потоковая генерация провайдеров

- [x] 2.1 `chat_models.cpp` (OpenAI-совместимый): `Model::turn_stream` с `StreamSink`, сборщик в `chat_stream.cpp` (текст, tool_calls по `index`, usage через `stream_options.include_usage`, `[DONE]`, фильтр `<think>`), итог через прежний `parse_openai_response`
- [x] 2.2 `chat_models.cpp` (Claude): `content_block_delta`, `input_json_delta`, `thinking`/`signature` в `raw`, `message_delta` с usage; итог через `parse_anthropic_response`
- [x] 2.3 Дедлайн: `timeout_ms` уже остаток дедлайна задания, поэтому `set_max_timeout` остаётся потолком всего потока; отмена — `StreamSink::should_stop` на каждом куске
- [x] 2.4 Тесты на локальном сервере (`tests/test_chat_stream.cpp`): куски текста, `<think>`, tool_call по частям, обрыв посреди ответа, отмена, ошибка 401 и сервер без stream, Claude (thinking, tool_use, error-событие); фейки цикла получили умолчательный `turn_stream`
- [x] 2.5 `parse_expense` и импорт не тронуты: `llm_client.cpp` по-прежнему с `stream:false`

## 3. Цикл chat публикует события

- [x] 3.1 `chat.cpp` через `chat::Hooks`: `delta` и `tool` (имя и исход; выдуманное имя — `unknown`); `done` и `error` ставит `Store::complete()` атомарно со статусом; итог и статусы не меняются
- [x] 3.2 Тесты цикла: ответ с инструментом, отказ модели, отмена, никто не читает поток
- [x] 3.3 Проверка приватности: в журнале сервиса и метриках нет текста `delta`

## 4. Маршрут /events

- [x] 4.1 `ai.cpp`: `GET /api/ai/jobs/{id}/events` через chunked content provider, `Content-Type: text/event-stream`, `Cache-Control: no-cache`, 404 для чужого, истёкшего и не-chat заданий
- [x] 4.2 `Last-Event-ID`, закрытие потока после `done`/`error`, пульс `: ping` раз в 15 с
- [x] 4.3 Лимиты `AI_STREAM_MAX`, `AI_STREAM_MAX_PER_USER` (429 `busy`), размер пула потоков в `main.cpp`, флаг `AI_STREAM_ENABLED`
- [x] 4.4 Метрики потоков (открыто, события, длительность, исход) без текста; запись о закрытии в журнал
- [x] 4.5 Тесты маршрута: свой/чужой, переподключение без пропусков и повторов, завершённое задание, отмена на лету, лимит потоков

## 5. Контракт и документация

- [x] 5.1 `openapi.yaml`: операция `jobEvents`, схемы событий; примечание про склейку `delta` и закрытие блока по `tool`
- [x] 5.2 `README.md`: переменные `AI_STREAM_*`, пример `curl -N`

## 6. Проверка

- [x] 6.1 Сборка и все тесты в Docker, по одному сервису за раз
- [x] 6.2 Прогон на test (2026-11-20, rc.2, через гейтвей rc.2, qwen3-8b): поток с LM Studio, переподключение по `Last-Event-ID` без пропусков, обрыв клиента задание не отменяет (`done`), 404 на чужой id. Отмена `DELETE` на лету покрыта юнит-тестами, на test не гонялась
- [x] 6.3 Замер: ответ ~100 знаков = 31 событие (28 `delta` с шагом ~40 мс, 2 `tool`, `done`); при медленном читателе события сливаются (8 вместо ~30). Первый байт — через 12 с (обработка промпта на GPU). Пул и лимиты (`AI_STREAM_MAX=20`, на пользователя 3) пока не нагружали, значения оставлены

## 7. Зависимости в других репозиториях (отдельные изменения)

- [x] 7.1 wallet-gateway (proxy-sse-stream, wallet-gateway#21, rc.2 на test): потоковый прокси для `text/event-stream` (блокирует выкладку SSE во внешний трафик)
- [ ] 7.2 wallet-telegram, wallet-web, android-wallet: SSE-клиенты
- [ ] 7.3 wallet-infra: переменные `AI_STREAM_*`

## 8. Окно потока (по итогам проверки бота на test)

- [x] 8.1 `GET /events?window_ms=` (100..60000): закрытие по окну без `done`, слот освобождается сразу; неверное значение — 400
- [x] 8.2 Тест: окно закрывает поток, продолжение с `Last-Event-ID`; `openapi.yaml`, README, спека
