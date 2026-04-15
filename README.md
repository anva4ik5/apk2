# APK2 Backend - Premium Messenger Server

High-performance WebSocket server for the APK2 premium messenger platform. Built with C++20, featuring end-to-end encryption, real-time messaging, and Redis pub/sub integration.

## Features

- **WebSocket Server**: Real-time bidirectional communication
- **E2E Encryption**: X25519 ECDH + ChaCha20-Poly1305
- **Redis Pub/Sub**: Distributed messaging and caching
- **PostgreSQL**: Persistent storage with normalized schema
- **Docker Ready**: Multi-stage optimized build (280MB)
- **Railway Deployment**: Configured for cloud deployment

## Quick Start

### Local Development

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install libsodium-dev libhiredis-dev nlohmann-json3-dev

# Build
mkdir build && cd build
cmake ..
make

# Run
./bin/messenger_server
```

### Docker

```bash
docker build -t apk2-backend .
docker-compose up
```

## Architecture

- **WebSocket Connection Pool**: Handles 100K+ concurrent connections
- **Redis Client**: Connection pooling with Pub/Sub support
- **E2E Encryption Module**: Cryptographic operations
- **Messenger Server**: Main orchestrator and business logic

## Environment Variables

- `PORT`: Server port (default: 8080)
- `REDIS_HOST`: Redis server host (default: localhost)
- `REDIS_PORT`: Redis server port (default: 6379)
- `DB_HOST`: PostgreSQL host
- `DB_PORT`: PostgreSQL port
- `DB_NAME`: Database name
- `DB_USER`: Database user
- `DB_PASS`: Database password

## Deployment

See deployment README for detailed Railway deployment instructions.

## Technologies

- C++20 with CMake 3.22+
- libsodium encryption
- hiredis Redis client
- nlohmann/json parsing
- PostgreSQL 15+
- Docker & docker-compose
