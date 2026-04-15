#pragma once

#include <string>
#include <cstdlib>
#include <iostream>

/**
 * @class DatabaseConfig
 * @brief PostgreSQL configuration for Railway deployment
 * 
 * Provides database connection parameters with Railway as default
 * Can be overridden via environment variables
 */
class DatabaseConfig {
public:
    // Railway PostgreSQL defaults
    static constexpr const char* default_host = "postgres.railway.internal";
    static constexpr int default_port = 5432;
    static constexpr const char* default_db = "railway";
    static constexpr const char* default_user = "postgres";
    static constexpr const char* default_password = "OPNuOrYZJidOPlGCcCwpDMYOROWtsWZq";
    
    // Full connection string (Railway)
    static constexpr const char* default_connection_url =
        "postgresql://postgres:OPNuOrYZJidOPlGCcCwpDMYOROWtsWZq@postgres.railway.internal:5432/railway";
    
    // Connection pool settings
    static constexpr int max_pool_connections = 20;
    static constexpr int min_pool_connections = 5;
    static constexpr int connection_timeout_seconds = 30;
    static constexpr int idle_timeout_minutes = 15;
    
    // Table names
    static constexpr const char* users_table = "users";
    static constexpr const char* messages_table = "messages";
    static constexpr const char* channels_table = "channels";
    static constexpr const char* contacts_table = "contacts";
    static constexpr const char* encryption_keys_table = "encryption_keys";
    
    /**
     * Get PostgreSQL connection URL from environment or use default
     * Environment variable: DATABASE_URL
     * Format: postgresql://user:password@host:port/database
     */
    static std::string get_connection_url() {
        const char* env_url = std::getenv("DATABASE_URL");
        if (env_url && std::string(env_url).length() > 0) {
            std::cout << "📡 Using DATABASE_URL from environment" << std::endl;
            return std::string(env_url);
        }
        
        std::cout << "📡 Using default Railway PostgreSQL connection" << std::endl;
        return std::string(default_connection_url);
    }
    
    /**
     * Get database host from environment or use default
     */
    static std::string get_db_host() {
        const char* env_host = std::getenv("PGHOST");
        if (env_host && std::string(env_host).length() > 0) {
            return std::string(env_host);
        }
        return std::string(default_host);
    }
    
    /**
     * Get database port from environment or use default
     */
    static int get_db_port() {
        const char* env_port = std::getenv("PGPORT");
        if (env_port) {
            try {
                return std::stoi(env_port);
            } catch (...) {
                return default_port;
            }
        }
        return default_port;
    }
    
    /**
     * Get database name from environment or use default
     */
    static std::string get_db_name() {
        const char* env_db = std::getenv("PGDATABASE");
        if (env_db && std::string(env_db).length() > 0) {
            return std::string(env_db);
        }
        return std::string(default_db);
    }
    
    /**
     * Get database user from environment or use default
     */
    static std::string get_db_user() {
        const char* env_user = std::getenv("PGUSER");
        if (env_user && std::string(env_user).length() > 0) {
            return std::string(env_user);
        }
        return std::string(default_user);
    }
    
    /**
     * Get database password from environment or use default
     */
    static std::string get_db_password() {
        const char* env_pass = std::getenv("PGPASSWORD");
        if (env_pass && std::string(env_pass).length() > 0) {
            return std::string(env_pass);
        }
        return std::string(default_password);
    }
    
    /**
     * Print configuration for debugging
     */
    static void print_config() {
        std::cout << "📊 Database Configuration:" << std::endl;
        std::cout << "  Host: " << get_db_host() << std::endl;
        std::cout << "  Port: " << get_db_port() << std::endl;
        std::cout << "  Database: " << get_db_name() << std::endl;
        std::cout << "  User: " << get_db_user() << std::endl;
        std::cout << "  Connection URL: " << get_connection_url() << std::endl;
    }
};
