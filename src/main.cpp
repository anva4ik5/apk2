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
    std::string db_url     = env("DATABASE_URL",
        "postgresql://postgres:OPNuOrYZJidOPlGCcCwpDMYOROWtsWZq"
        "@postgres.railway.internal:5432/railway");
    
    std::string redis_host;
    int         redis_port = 6379;
    
    std::string redishost_env = env("REDISHOST", "");
    std::string redisport_env = env("REDISPORT", "");
    
    if (!redishost_env.empty()) {
        redis_host = redishost_env;
        if (!redisport_env.empty()) {
            try {
                redis_port = std::stoi(redisport_env);
            } catch (...) {
                redis_port = 6379;
            }
        }
    } else {
        std::string redis_url = env("REDIS_URL", "");
        if (redis_url.empty()) {
            redis_url = "redis://redis:6379";
        }

        std::string url = redis_url;
        auto scheme_pos = url.find("://");
        if (scheme_pos != std::string::npos) {
            url = url.substr(scheme_pos + 3);
        }
        auto slash_pos = url.find('/');
        if (slash_pos != std::string::npos) {
            url = url.substr(0, slash_pos);
        }
        auto colon_pos = url.rfind(':');
        if (colon_pos != std::string::npos && colon_pos + 1 < url.size()) {
            redis_host = url.substr(0, colon_pos);
            try {
                redis_port = std::stoi(url.substr(colon_pos + 1));
            } catch (...) {
                redis_port = 6379;
            }
        } else {
            redis_host = url.empty() ? "redis" : url;
            redis_port = 6379;
        }
    }

    uint16_t    http_port  = 8080;
    try {
        http_port = (uint16_t)std::stoi(env("PORT", "8080"));
    } catch (...) {
        http_port = 8080;
    }
    uint16_t    ws_port    = 8000;
    try {
        ws_port = (uint16_t)std::stoi(env("WS_PORT", "8000"));
    } catch (...) {
        ws_port = 8000;
    }
    std::string jwt_secret = env("JWT_SECRET", "change_me_in_production_please");
    std::string log_level  = env("LOG_LEVEL",  "info");

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--port"   && i+1 < argc) http_port  = (uint16_t)std::stoi(argv[++i]);
        else if (arg == "--ws-port"&& i+1 < argc) ws_port    = (uint16_t)std::stoi(argv[++i]);
        else if (arg == "--db-url" && i+1 < argc) db_url     = argv[++i];
        else if (arg == "--redis"  && i+1 < argc) redis_host = argv[++i];
        else if (arg == "--jwt"    && i+1 < argc) jwt_secret = argv[++i];
    }

    std::cout << "[config] HTTP=" << http_port
              << " WS=" << ws_port
              << " Redis=" << redis_host << ":" << redis_port << std::endl;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    // --- HTTP REST API (background thread) ---
    HttpApi::Config http_cfg;
    http_cfg.port       = http_port;
    http_cfg.db_url     = db_url;
    http_cfg.redis_host = redis_host;
    http_cfg.redis_port = redis_port;
    http_cfg.jwt_secret = jwt_secret;

    HttpApi http_api(http_cfg);
    g_http_api = &http_api;

    std::thread http_thread([&http_api]() {
        try { http_api.run(); }
        catch (const std::exception& e) {
            std::cerr << "[HTTP] Error: " << e.what() << std::endl;
        }
    });

    // --- WebSocket Server (main thread) ---
    MessengerServer::Config ws_cfg;
    ws_cfg.db_connection = db_url;
    ws_cfg.redis_host    = redis_host;
    ws_cfg.redis_port    = redis_port;
    ws_cfg.ws_port       = ws_port;
    ws_cfg.port          = 0;
    ws_cfg.log_level     = log_level;

    MessengerServer ws_server(ws_cfg);
    g_ws_server = &ws_server;

    int startup_attempts = 0;
    while (!ws_server.start() && startup_attempts < 10) {
        std::cerr << "[WS] Failed to start, retrying in 5 seconds... (attempt " 
                  << ++startup_attempts << "/10)" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    if (!ws_server.is_running()) {
        std::cerr << "[WS] Failed to start after retries, continuing with HTTP server only" << std::endl;
    }

    std::cout << "[ok] HTTP server running on port " << http_port 
              << ". WS server status: " << (ws_server.is_running() ? "running" : "degraded") 
              << ". Ctrl+C to stop." << std::endl;

    int tick = 0;
    while (ws_server.is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++tick % 60 == 0) {
            auto s = ws_server.get_server_stats();
            std::cout << "[stats] conn=" << s["total_connections"]
                      << " online=" << s["online_users"] << std::endl;
        }
    }

    http_api.stop();
    http_thread.join();
    return 0;
}
