#pragma once

#include <memory>
#include <string>
#include <functional>
#include "redis_client.hpp"
#include "websocket_connection.hpp"
#include "e2e_encryption.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @class MessengerServer
 * @brief Main server class orchestrating WebSocket, Redis, and message routing
 * 
 * Responsibilities:
 * - Accept WebSocket connections
 * - Handle authentication
 * - Route messages between users via Redis Pub/Sub
 * - Manage user sessions and presence
 */
class MessengerServer {
public:
    struct Config {
        std::string host = "0.0.0.0";
        uint16_t port = 8080;
        uint16_t ws_port = 8000;
        
        std::string redis_host = "localhost";
        int redis_port = 6379;
        
        // PostgreSQL Configuration (Railway)
        std::string db_connection = "postgresql://postgres:OPNuOrYZJidOPlGCcCwpDMYOROWtsWZq@postgres.railway.internal:5432/railway";
        
        std::string log_level = "info";  // debug, info, warn, error
        int worker_threads = 4;
        int max_connections = 100000;
        
        bool enable_ssl = false;
        std::string ssl_cert_path;
        std::string ssl_key_path;
    };

    explicit MessengerServer(const Config& config);
    ~MessengerServer();

    // Server Lifecycle
    bool start();
    void stop();
    bool is_running() const { return running_; }

    // Connection Management
    void on_client_connected(int connection_id);
    void on_client_disconnected(int connection_id);
    void on_client_message(int connection_id, const std::string& raw_data);

    // Message Handling
    void handle_message(int connection_id, const json& message);
    void handle_authentication(int connection_id, const json& auth_data);
    void handle_typing_indicator(int connection_id, const json& data);
    void handle_read_receipt(int connection_id, const json& data);

    // Broadcast & Publishing
    void send_to_user(const std::string& user_id, const WebSocketConnection::Message& msg);
    void send_to_group(const std::vector<std::string>& user_ids, const WebSocketConnection::Message& msg);

    // Server Status
    json get_server_stats() const;
    json get_user_status(const std::string& user_id) const;

    // Pool accessor for WebSocket flush
    ConnectionPool* get_connection_pool() { return connection_pool_.get(); }

private:
    struct PendingMessage {
        std::string sender_id;
        std::string recipient_id;
        std::vector<uint8_t> encrypted_content;
        uint64_t timestamp;
        uint64_t message_id;
    };

    Config config_;
    std::atomic<bool> running_{false};
    
    std::unique_ptr<RedisClient> redis_;
    std::unique_ptr<ConnectionPool> connection_pool_;
    
    // E2E Encryption
    E2EEncryption::KeyPair server_keys_;
    std::map<std::string, E2EEncryption::DoubleRatchet> user_ratchets_;
    mutable std::mutex ratchet_mutex_;

    // Redis Pub/Sub subscriptions
    void subscribe_to_user_channel(const std::string& user_id);
    void redis_message_handler(const std::string& channel, const std::string& message);
    
    // Message Processing
    bool process_encrypted_message(const json& message, std::string& decrypted_content);
    std::string encrypt_for_delivery(const std::string& content, const std::string& recipient_id);
    
    // Database operations
    bool authenticate_user(const std::string& username, const std::string& password, std::string& user_id, std::string& session_token);
    bool validate_session(const std::string& session_token, std::string& user_id);
    void store_session(const std::string& user_id, const std::string& session_token);
    void mark_user_online(const std::string& user_id);
    void mark_user_offline(const std::string& user_id);

    // Event handlers
    void on_redis_connected();
    void on_redis_disconnected();
    void on_message_delivered(const std::string& message_id, const std::string& recipient_id);

    // Logging
    void log(const std::string& level, const std::string& message);
    std::string get_timestamp() const;
};
