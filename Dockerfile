# ============================================================
# Stage 1 — Builder
# ============================================================
FROM ubuntu:22.04 AS builder

# Добавлены libasio-dev и python3 для корректной сборки
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libsodium-dev \
    libhiredis-dev \
    libssl-dev \
    libpq-dev \
    libasio-dev \
    python3 \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# Собираем проект
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

# ============================================================
# Stage 2 — Runtime
# ============================================================
FROM ubuntu:22.04

# Устанавливаем только необходимые runtime-библиотеки + psql для миграций
RUN apt-get update && apt-get install -y \
    libsodium23 \
    libhiredis0.14 \
    libssl3 \
    libpq5 \
    postgresql-client \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -u 1000 messenger && mkdir -p /app/data && chown messenger:messenger /app/data
WORKDIR /app

# Копируем бинарник, миграцию и entrypoint
COPY --from=builder --chown=messenger:messenger /build/build/bin/messenger_server /app/server
COPY --chown=messenger:messenger database/migration_http_api.sql /app/migration.sql
COPY --chown=messenger:messenger entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

USER messenger

ENV PORT=8080 \
    WS_PORT=8000 \
    REDIS_HOST=localhost \
    REDIS_PORT=6379 \
    LOG_LEVEL=info \
    JWT_SECRET=change_me_in_production_please

EXPOSE 8080 8000

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD curl -sf http://localhost:${PORT}/health || exit 1

CMD ["/app/entrypoint.sh"]
