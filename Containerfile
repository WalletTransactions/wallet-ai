# syntax=docker/dockerfile:1
#
# Multi-stage build wallet-ai — тот же шаблон, что у wallet-telegram:
# deps -> build (образ), deps -> test (юнит-тесты, `--target test`), runtime.
# libpq — база ai (импорт выписок), poppler-utils — pdftotext для PDF-выписок.
# curl в рантайме — для healthcheck в compose.

FROM debian:bookworm-slim AS deps

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libpq-dev \
        libssl-dev \
        pkg-config \
        poppler-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY VERSION ./
COPY openapi.yaml ./
COPY prompts/ prompts/
COPY src/ src/
COPY third_party/ third_party/

FROM deps AS build
# GIT_COMMIT — снаружи (`--build-arg`, см. compose.yaml): у сборки в
# контейнере нет .git.
ARG GIT_COMMIT=unknown
ARG BUILD_JOBS=3
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DWALLET_AI_BUILD_TESTS=OFF -DGIT_COMMIT="${GIT_COMMIT}" \
    && cmake --build build --config Release -j"${BUILD_JOBS}"

FROM deps AS test
COPY tests/ tests/
ARG BUILD_JOBS=3
RUN cmake -B build-tests -DCMAKE_BUILD_TYPE=Debug \
    && cmake --build build-tests --target wallet_ai_tests -j"${BUILD_JOBS}" \
    && ./build-tests/tests/wallet_ai_tests

FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libssl3 \
        libpq5 \
        poppler-utils \
        curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /app --shell /usr/sbin/nologin appuser

WORKDIR /app
COPY --from=build /src/build/wallet_ai /app/wallet_ai
# openapi.yaml + Swagger UI читаются с диска при GET /openapi.yaml, /docs.
COPY openapi.yaml /app/openapi.yaml
# Промпты — тоже с диска при старте (LLM_PROMPT_DIR, по умолчанию prompts/).
COPY prompts/ /app/prompts/
COPY third_party/swagger-ui/ /app/vendor/swagger-ui/
# Миграции своей базы. Накатывает их wallet-infra/infra/env/migrate.sh из
# ЭТОГО образа (копирует /migrations), а не из рабочей папки: схема всегда
# той же версии, что и код, который на неё выкатывается.
COPY migrations/ /migrations/

USER appuser
EXPOSE 8087

ENTRYPOINT ["/app/wallet_ai"]
