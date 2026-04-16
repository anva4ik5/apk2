#include "messenger_server.hpp"
#include "http_api.hpp"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <cstdlib>

static MessengerServer* g_ws_server  = nullptr;
static HttpApi*         g_http_api   = nullptr;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n\n Shutting down..." << std::endl;
        if (g_http_api)   g_http_api->stop();
        if (g_ws_server)  g_ws_server->stop();
        exit(0);
    }
}

static std::string env(const char* key, const char* fallback = "") {
    const char* v = std::getenv(key);
    return (v && v[0]) ? std::string(v) : std::string(fallback);
}

int main(int argc, char* argv[]) {
    std::string db_url = env("DATABASE_URL",
        "postgresql://postgres:OPNuOrYZJidOPlGCcCwpDMYOROWtsWZq"
        "@postgres.railway.internal:5432/railway");

    std::string redis_host;
    int         redis_port = 6379;

    std::string redishost_env = env("REDISHOST", "");
    std::string redisport_env = env("REDISPORT", "");

    if (!redishost_env.empty()) {
        redis_host = redishost_env;
        if (!redisport_env.empty()) {
            try { redis_port = std::stoi(redisport_env); } catch (...) { redis_port = 6379; }
        }
    } else {
        std::string redis_url = env("REDIS_URL", "");
        if (redis_url.empty()) redis_url = "redis://redis:6379";

        std::string url = redis_url;
        auto scheme_pos = url.find("://");
        if (scheme_pos != std::string::npos) url = url.substr(scheme_pos + 3);
        auto slash_pos = url.find('/');
        if (slash_pos != std::string::npos) url = url.substr(0, slash_pos);
        auto colon_pos = url.rfind(':');
        if (colon_pos != std::string::npos && colon_pos + 1 < url.size()) {
            redis_host = url.substr(0, colon_pos);
            try { redis_port = std::stoi(url.substr(colon_pos + 1)); } catch (...) { redis_port = 6379; }
        } else {
            redis_host = url.empty() ? "redis" : url;
            redis_port = 6379;
        }
    }

    uint16_t http_port = 8080;
    try { http_port = (uint16_t)std::stoi(env("PORT", "8080")); } catch (...) { http_port = 8080; }

    uint16_t ws_port = 8000;
    try { ws_port = (uint16_t)std::stoi(env("WS_PORT", "8000")); } catch (...) { ws_port = 8000; }

    std::string jwt_secret = env("JWT_SECRET", "change_me_in_production_please");
    std::string log_level  = env("LOG_LEVEL",  "info");

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--port"    && i+1 < argc) http_port  = (uint16_t)std::stoi(argv[++i]);
        else if (arg == "--ws-port" && i+1 < argc) ws_port    = (uint16_t)std::stoi(argv[++i]);
        else if (arg == "--db-url"  && i+1 < argc) db_url     = argv[++i];
        else if (arg == "--redis"   && i+1 < argc) redis_host = argv[++i];
        else if (arg == "--jwt"     && i+1 < argc) jwt_secret = argv[++i];
    }

    std::cout << "[config] HTTP=" << http_port
              << " WS=" << ws_port
              << " Redis=" << redis_host << ":" << redis_port << std::endl;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    // --- WS сервер запускается В ФОНЕ чтобы не блокировать HTTP ---
    MessengerServer::Config ws_cfg;
    ws_cfg.db_connection = db_url;
    ws_cfg.redis_host    = redis_host;
    ws_cfg.redis_port    = redis_port;
    ws_cfg.ws_port       = ws_port;
    ws_cfg.port          = 0;
    ws_cfg.log_level     = log_level;

    // Создаём на heap чтобы lifetime был до конца программы
    MessengerServer* ws_server = new MessengerServer(ws_cfg);
    g_ws_server = ws_server;

    std::thread ws_thread([ws_server]() {
        // Ждём 3 сек — даём HTTP подняться и пройти healthcheck
        std::this_thread::sleep_for(std::chrono::seconds(3));
        int attempts = 0;
        while (!ws_server->start() && attempts < 5) {
            std::cerr << "[WS] Retry " << ++attempts << "/5..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        if (ws_server->is_running()) {
            std::cout << "[WS] Started successfully" << std::endl;
        } else {
            std::cerr << "[WS] Degraded mode (no WS)" << std::endl;
        }
    });
    ws_thread.detach();

    // --- HTTP запускается в главном потоке — блокирует до остановки ---
    // Railway healthcheck увидит /health сразу после старта Crow
    HttpApi::Config http_cfg;
    http_cfg.port       = http_port;
    http_cfg.db_url     = db_url;
    http_cfg.redis_host = redis_host;
    http_cfg.redis_port = redis_port;
    http_cfg.jwt_secret = jwt_secret;

    HttpApi http_api(http_cfg);
    g_http_api = &http_api;

    // Wire WebSocket messenger into HTTP so /ws route works on the same port
    http_api.set_messenger(ws_server);

    std::cout << "[ok] Starting HTTP on port " << http_port << std::endl;

    try {
        http_api.run(); // блокирует — это нормально
    } catch (const std::exception& e) {
        std::cerr << "[HTTP] Fatal: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
