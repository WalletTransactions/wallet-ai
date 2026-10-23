#!/usr/bin/env bash
# Поднимает LLM для wallet-ai в LM Studio на хосте: сервер на порту, доступный
# из docker, и модель под стабильным id (LLM_MODEL в wallet-ai/.env).
# Идемпотентен: уже запущенный сервер и уже загруженную модель не трогает.
#
#   bash wallet-ai/scripts/lms-load.sh
#   LMS_MODEL_KEY=qwen/qwen3-8b bash wallet-ai/scripts/lms-load.sh    # другая модель
#
# Переменные (все необязательные):
#   LMS_BIN          путь к lms                    (~/.lmstudio/bin/lms)
#   LMS_MODEL_KEY    модель из `lms ls`            (google/gemma-4-e4b)
#   LMS_IDENTIFIER   id в API = LLM_MODEL          (wallet-parse)
#   LMS_CONTEXT      контекст, токенов             (8192 — = LLM_CONTEXT wallet-ai;
#                    gemma-4-e4b с ним ~4,8 ГБ из 8 на GTX 1070)
#   LMS_GPU          доля на GPU: max/off/0..1     (max)
#   LMS_PORT         порт сервера                  (1234)
#   LMS_BIND         адрес сервера                 (0.0.0.0 — иначе из docker не достучаться;
#                    порт в файрволе открывать только для 172.16.0.0/12)
#
# Модель грузится без --ttl: авто-выгрузка по простою вернула бы JIT-загрузку
# (10–30 с) на первом запросе — дольше таймаута wallet-ai.
set -euo pipefail

LMS_BIN="${LMS_BIN:-$HOME/.lmstudio/bin/lms}"
LMS_MODEL_KEY="${LMS_MODEL_KEY:-google/gemma-4-e4b}"
LMS_IDENTIFIER="${LMS_IDENTIFIER:-wallet-parse}"
LMS_CONTEXT="${LMS_CONTEXT:-8192}"
LMS_GPU="${LMS_GPU:-max}"
LMS_PORT="${LMS_PORT:-1234}"
LMS_BIND="${LMS_BIND:-0.0.0.0}"

[[ -x "$LMS_BIN" ]] || { echo "lms не найден: $LMS_BIN (LMS_BIN=...)" >&2; exit 1; }

# 1. Сервер. `lms` сам будит демон LM Studio; первый вызов после загрузки
#    системы может занять до минуты.
if "$LMS_BIN" server status --json 2>/dev/null | grep -q '"running":true'; then
    echo "сервер уже запущен"
else
    echo "запускаю сервер на $LMS_BIND:$LMS_PORT"
    "$LMS_BIN" server start --port "$LMS_PORT" --bind "$LMS_BIND"
fi

# 2. Модель под нужным id.
if "$LMS_BIN" ps --json 2>/dev/null | grep -q "\"identifier\":\"$LMS_IDENTIFIER\""; then
    echo "модель $LMS_IDENTIFIER уже загружена"
else
    echo "загружаю $LMS_MODEL_KEY как $LMS_IDENTIFIER (контекст $LMS_CONTEXT, GPU $LMS_GPU)"
    "$LMS_BIN" load "$LMS_MODEL_KEY" --identifier "$LMS_IDENTIFIER" \
        --context-length "$LMS_CONTEXT" --gpu "$LMS_GPU" --yes
fi

# 3. Проверка тем же запросом, что проба wallet-ai (GET /v1/models).
#    LLM_API_KEY из окружения — если в LM Studio включён токен.
auth=()
[[ -n "${LLM_API_KEY:-}" ]] && auth=(-H "Authorization: Bearer $LLM_API_KEY")
if curl -fsS -m 10 "${auth[@]}" "http://127.0.0.1:$LMS_PORT/v1/models" | grep -q "\"$LMS_IDENTIFIER\""; then
    echo "ok: $LMS_IDENTIFIER доступна на :$LMS_PORT"
else
    echo "GET /v1/models не вернул $LMS_IDENTIFIER (токен LM Studio? LLM_API_KEY=...)" >&2
    exit 1
fi
