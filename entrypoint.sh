#!/bin/sh
set -e

# Запускаем миграцию если задана DATABASE_URL
if [ -n "$DATABASE_URL" ]; then
    echo "[entrypoint] Running database migration..."
    psql "$DATABASE_URL" -f /app/migration.sql && \
        echo "[entrypoint] Migration done." || \
        echo "[entrypoint] Migration warning (may already be applied, continuing...)"
else
    echo "[entrypoint] WARNING: DATABASE_URL not set, skipping migration."
fi

# Запускаем сервер
exec /app/server
