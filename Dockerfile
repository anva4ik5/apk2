# ============================================================================
# Dockerfile for Premium Messenger Server
# Target: Linux AMD64 (Railway, DigitalOcean, AWS)
# Build stages: Builder → Runtime (multi-stage optimization)
# ============================================================================

# Stage 1: Builder (Ubuntu 22.04 with build tools)
FROM ubuntu:22.04 AS builder

LABEL maintainer="Messenger Dev Team"
LABEL version="2.0.0"

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libsodium-dev \
    libhiredis-dev \
    libssl-dev \
    nlohmann-json3-dev \
    curl \
    wget \
    &&  rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Copy source code
COPY . .

# Create build directory and compile
RUN mkdir -p build && cd build && \
    cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER=gcc \
    &&  make -j$(nproc) && \
    make install DESTDIR=/app

# ============================================================================
# Stage 2: Runtime (minimal image)
FROM ubuntu:22.04

LABEL description="Premium Messenger WebSocket Server"
LABEL version="2.0.0"

# Install  runtime dependencies only
RUN apt-get update && apt-get install -y \
    libsodium23 \
    libhiredis0.14 \
    libssl3 \
    ca-certificates \
    curl \
    net-tools \
    dnsutils \
    &&  rm -rf /var/lib/apt/lists/*

# Create non-root user for security
RUN useradd -m -u 1000 messenger && \
    mkdir -p /app/data && \
    chown -R messenger:messenger /app

WORKDIR /app

# Copy compiled binary from builder
COPY --from=builder --chown=messenger:messenger /build/build/bin/messenger-server /app/messenger-server

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=40s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

# Switch to non-root user
USER messenger

# Environment variables (can be overridden by Railway secrets)
ENV HOST=0.0.0.0 \
    PORT=8080 \
    WS_PORT=8000 \
    REDIS_HOST=localhost \
    REDIS_PORT=6379 \
    LOG_LEVEL=info \
    WORKERS=4 \
    MAX_CONNECTIONS=100000

# Expose ports
EXPOSE 8080 8000

# Enable graceful shutdown
STOPSIGNAL SIGTERM

# Run server
CMD ["/app/messenger-server", \
     "--host", "${HOST}", \
     "--port", "${PORT}", \
     "--ws-port", "${WS_PORT}", \
     "--redis-host", "${REDIS_HOST}", \
     "--redis-port", "${REDIS_PORT}", \
     "--log-level", "${LOG_LEVEL}"]

# ============================================================================
# Building Instructions:
# 
# Local build:
#   docker build -t messenger-server:latest .
#   docker run -d \
#     -p 8080:8080 \
#     -p 8000:8000 \
#     -e REDIS_HOST=redis \
#     --name messenger \
#     messenger-server:latest
#
# Railway deployment:
#   - Push to GitHub
#   - Railway detects Dockerfile and auto-builds
#   - Set environment variables in Railway dashboard
#   - Configure Redis add-on for REDIS_HOST
#
# Docker Compose (with Redis):
#   docker-compose up -d
#
# ============================================================================
