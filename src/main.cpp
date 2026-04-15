#include "messenger_server.hpp"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>

// Global server instance (for signal handling)
static MessengerServer* g_server = nullptr;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n\n🛑 Shutting down server..." << std::endl;
        if (g_server) {
            g_server->stop();
        }
        exit(0);
    }
}

int main(int argc, char* argv[]) {
    std::cout << R"(
███╗   ███╗███████╗███████╗███████╗███████╗███╗   ██╗ ██████╗ ███████╗██████╗ 
████╗ ████║██╔════╝██╔════╝██╔════╝██╔════╝████╗  ██║██╔════╝ ██╔════╝██╔══██╗
██╔████╔██║█████╗  ███████╗███████╗█████╗  ██╔██╗ ██║██║  ███╗█████╗  ██████╔╝
██║╚██╔╝██║██╔══╝  ╚════██║╚════██║██╔══╝  ██║╚██╗██║██║   ██║██╔══╝  ██╔══██╗
██║ ╚═╝ ██║███████╗███████║███████║███████╗██║ ╚████║╚██████╔╝███████╗██║  ██║
╚═╝     ╚═╝╚══════╝╚══════╝╚══════╝╚══════╝╚═╝  ╚═══╝ ╚═════╝ ╚══════╝╚═╝  ╚═╝
                        C++ WebSocket Server v2.0.0
    )" << std::endl;

    // Database Configuration (Railway PostgreSQL)
    // Format: postgresql://user:password@host:port/database
    // Default: postgresql://postgres:PASSWORD@postgres.railway.internal:5432/railway
    std::string db_connection = std::getenv("DATABASE_URL") 
        ? std::getenv("DATABASE_URL")
        : "postgresql://postgres:OPNuOrYZJidOPlGCcCwpDMYOROWtsWZq@postgres.railway.internal:5432/railway";
    
    // Server Configuration
    MessengerServer::Config config;
    config.db_connection = db_connection;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } 
        else if (arg == "--port" && i + 1 < argc) {
            config.port = std::stoul(argv[++i]);
        }
        else if (arg == "--ws-port" && i + 1 < argc) {
            config.ws_port = std::stoul(argv[++i]);
        }
        else if (arg == "--redis-host" && i + 1 < argc) {
            config.redis_host = argv[++i];
        }
        else if (arg == "--redis-port" && i + 1 < argc) {
            config.redis_port = std::stoi(argv[++i]);
        }
        else if (arg == "--log-level" && i + 1 < argc) {
            config.log_level = argv[++i];
        }
        else if (arg == "--db-url" && i + 1 < argc) {
            config.db_connection = argv[++i];
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << 
                "Usage: messenger-server [options]\n\n"
                "Options:\n"
                "  --host <IP>           HTTP server host (default: 0.0.0.0)\n"
                "  --port <PORT>         HTTP server port (default: 8080)\n"
                "  --ws-port <PORT>      WebSocket server port (default: 8000)\n"
                "  --redis-host <HOST>   Redis host (default: localhost)\n"
                "  --redis-port <PORT>   Redis port (default: 6379)\n"
                "  --db-url <URL>        PostgreSQL connection (default: from DATABASE_URL env or Railway)\n"
                "  --log-level <LEVEL>   Log level: debug, info, warn, error (default: info)\n"
                "  --help, -h            Show this help message\n\n"
                "Environment Variables:\n"
                "  PORT                  Server HTTP port (overrides --port)\n"
                "  DATABASE_URL          PostgreSQL connection string\n"
                << std::endl;
            return 0;
        }
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Log database configuration
    std::cout << "\n📊 Server Configuration:" << std::endl;
    std::cout << "  Host: " << config.host << std::endl;
    std::cout << "  Port: " << config.port << std::endl;
    std::cout << "  WebSocket Port: " << config.ws_port << std::endl;
    std::cout << "  Redis: " << config.redis_host << ":" << config.redis_port << std::endl;
    std::cout << "  Database: " << config.db_connection.substr(0, 40) << "..." << std::endl << std::endl;

    // Create and start server
    MessengerServer server(config);
    g_server = &server;

    if (!server.start()) {
        std::cerr << "❌ Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "✅ Server is running. Press Ctrl+C to stop." << std::endl;

    // Main loop - keep server running
    while (server.is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Periodically log stats
        static int ticker = 0;
        if (++ticker % 30 == 0) {  // Every 30 seconds
            auto stats = server.get_server_stats();
            std::cout << "📊 Stats: " 
                     << "Connections=" << stats["total_connections"] 
                     << ", Online=" << stats["online_users"]
                     << ", Redis=" << (stats["redis_connected"] ? "OK" : "FAIL")
                     << std::endl;
        }

        // Cleanup inactive connections (every minute)
        if (ticker % 60 == 0) {
            std::cout << "🧹 Cleaning up inactive connections..." << std::endl;
        }
    }

    std::cout << "👋 Server stopped" << std::endl;
    return 0;
}
