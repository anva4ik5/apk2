#include "messenger_server.hpp"
#include <iostream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>

MessengerServer::MessengerServer(const Config& config) 
    : config_(config) {
    
    // Initialize encryption
    E2EEncryption::initialize();
    server_keys_ = E2EEncryption::generate_keypair();
    
    // Initialize Redis client
    RedisClient::Config redis_config;
    redis_config.host = config_.redis_host;
    redis_config.port = config_.redis_port;
    redis_config.pool_size = 32;
    
    redis_ = std::make_unique<RedisClient>(redis_config);
    
    // Initialize connection pool
    connection_pool_ = std::make_unique<ConnectionPool>();
    
    log("info", "MessengerServer initialized");
}

MessengerServer::~MessengerServer() {
    stop();
}

bool MessengerServer::start() {
    if (running_) {
        return true;
    }
    
    // Connect to Redis
    if (!redis_->connect()) {
        log("error", "Failed to connect to Redis");
        return false;
    }
    
    // Set up error callback
    redis_->set_error_callback([this](const std::string& error) {
        log("error", "Redis error: " + error);
    });
    
    running_ = true;
    
    log("info", "MessengerServer started");
    log("info", "WebSocket listening on " + config_.host + ":" + std::to_string(config_.ws_port));
    log("info", "HTTP API listening on " + config_.host + ":" + std::to_string(config_.port));
    
    return true;
}

void MessengerServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (redis_) {
        redis_->disconnect();
    }
    
    log("info", "MessengerServer stopped");
}

void MessengerServer::on_client_connected(int connection_id) {
    auto conn = connection_pool_->create_connection(connection_id);
    if (!conn) {
        log("error", "Failed to create connection for client " + std::to_string(connection_id));
        return;
    }
    
    conn->mark_connected();
    
    log("info", "Client connected: " + std::to_string(connection_id) + 
               " (Total: " + std::to_string(connection_pool_->get_total_connections()) + ")");
}

void MessengerServer::on_client_disconnected(int connection_id) {
    auto conn = connection_pool_->get_connection(connection_id);
    if (conn && !conn->get_user_id().empty()) {
        mark_user_offline(conn->get_user_id());
        
        // Publish user offline event
        WebSocketConnection::Message offline_msg;
        offline_msg.type = "user_offline";
        offline_msg.payload = {
            {"user_id", conn->get_user_id()},
            {"timestamp", std::time(nullptr)}
        };
        
        redis_->publish("user_status", offline_msg.payload.dump());
    }
    
    connection_pool_->remove_connection(connection_id);
    
    log("info", "Client disconnected: " + std::to_string(connection_id) + 
               " (Total: " + std::to_string(connection_pool_->get_total_connections()) + ")");
}

void MessengerServer::on_client_message(int connection_id, const std::string& raw_data) {
    try {
        auto config = json::parse(raw_data);
        handle_message(connection_id, config);
    } catch (const json::parse_error& e) {
        log("error", "JSON parse error: " + std::string(e.what()));
        
        // Send error response
        WebSocketConnection::Message error_msg;
        error_msg.type = "error";
        error_msg.payload = {
            {"message", "Invalid message format"},
            {"code", "INVALID_FORMAT"}
        };
        
        auto conn = connection_pool_->get_connection(connection_id);
        if (conn) {
            conn->queue_message(error_msg);
        }
    }
}

void MessengerServer::handle_message(int connection_id, const json& message) {
    auto conn = connection_pool_->get_connection(connection_id);
    if (!conn) {
        log("error", "Connection not found: " + std::to_string(connection_id));
        return;
    }
    
    std::string type = message.value("type", "unknown");
    
    if (type == "auth") {
        handle_authentication(connection_id, message);
    } 
    else if (type == "message") {
        if (!conn->is_authenticated()) {
            WebSocketConnection::Message err;
            err.type = "error";
            err.payload = {{"message", "Not authenticated"}};
            conn->queue_message(err);
            return;
        }
        
        // Handle encrypted message
        std::string sender_id = conn->get_user_id();
        std::string recipient_id = message.value("to", "");
        std::string encrypted_content = message.value("content", "");
        uint64_t msg_id = message.value("id", 0ULL);
        
        // Store in Redis queue for recipient
        std::string queue_key = "queue:messages:" + recipient_id;
        redis_->lpush(queue_key, {encrypted_content});
        redis_->expire(queue_key, 86400);  // TTL: 24 hours
        
        // Publish to Pub/Sub for real-time delivery
        std::string channel = "user:" + recipient_id;
        json pubsub_msg = {
            {"type", "message"},
            {"from", sender_id},
            {"message_id", msg_id},
            {"content", encrypted_content},
            {"timestamp", std::time(nullptr)}
        };
        
        redis_->publish(channel, pubsub_msg.dump());
        
        // Send delivery confirmation to sender
        WebSocketConnection::Message delivered;
        delivered.type = "message_delivered";
        delivered.payload = {
            {"message_id", msg_id},
            {"status", "queued"}
        };
        conn->queue_message(delivered);
        
        log("info", "Message queued: " + sender_id + " -> " + recipient_id + " (ID: " + std::to_string(msg_id) + ")");
    }
    else if (type == "typing") {
        handle_typing_indicator(connection_id, message);
    }
    else if (type == "read") {
        handle_read_receipt(connection_id, message);
    }
    else {
        log("warn", "Unknown message type: " + type);
    }
}

void MessengerServer::handle_authentication(int connection_id, const json& auth_data) {
    auto conn = connection_pool_->get_connection(connection_id);
    if (!conn) {
        return;
    }
    
    std::string username = auth_data.value("username", "");
    std::string password = auth_data.value("password", "");
    
    std::string user_id, session_token;
    
    if (authenticate_user(username, password, user_id, session_token)) {
        conn->set_user_id(user_id);
        conn->set_session_id(session_token);
        conn->mark_authenticated();
        
        mark_user_online(user_id);
        
        // Subscribe to this user's message channel
        subscribe_to_user_channel(user_id);
        
        // Initialize Double Ratchet for this user
        std::lock_guard<std::mutex> lock(ratchet_mutex_);
        ratchet_mutex_.lock();
        user_ratchets_[user_id] = E2EEncryption::DoubleRatchet(
            E2EEncryption::compute_shared_secret(server_keys_.private_key, server_keys_.public_key)
        );
        ratchet_mutex_.unlock();
        
        WebSocketConnection::Message auth_response;
        auth_response.type = "auth_success";
        auth_response.payload = {
            {"user_id", user_id},
            {"session_token", session_token},
            {"server_public_key", E2EEncryption::bytes_to_hex(
                std::vector<uint8_t>(server_keys_.public_key.begin(), server_keys_.public_key.end())
            )},
            {"timestamp", std::time(nullptr)}
        };
        
        conn->queue_message(auth_response);
        
        log("info", "User authenticated: " + user_id + " (Session: " + session_token.substr(0, 16) + "...)");
    } else {
        WebSocketConnection::Message auth_error;
        auth_error.type = "auth_failed";
        auth_error.payload = {
            {"message", "Invalid credentials"},
            {"code", "INVALID_AUTH"}
        };
        
        conn->queue_message(auth_error);
        
        log("warn", "Authentication failed for user: " + username);
    }
}

void MessengerServer::handle_typing_indicator(int connection_id, const json& data) {
    auto conn = connection_pool_->get_connection(connection_id);
    if (!conn || !conn->is_authenticated()) {
        return;
    }
    
    std::string user_id = conn->get_user_id();
    std::string chat_id = data.value("chat_id", "");
    bool typing = data.value("typing", false);
    
    conn->set_typing(typing);
    
    // Publish typing status
    json typing_msg = {
        {"type", "user_typing"},
        {"user_id", user_id},
        {"chat_id", chat_id},
        {"typing", typing}
    };
    
    redis_->publish("chat:" + chat_id, typing_msg.dump());
}

void MessengerServer::handle_read_receipt(int connection_id, const json& data) {
    auto conn = connection_pool_->get_connection(connection_id);
    if (!conn || !conn->is_authenticated()) {
        return;
    }
    
    std::string user_id = conn->get_user_id();
    std::string message_id = data.value("message_id", "");
    
    // Update message status in Redis
    redis_->hset("message_status:" + message_id, "read_by:" + user_id, std::to_string(std::time(nullptr)));
}

void MessengerServer::send_to_user(const std::string& user_id, const WebSocketConnection::Message& msg) {
    auto conn = connection_pool_->find_by_user(user_id);
    if (conn && conn->is_connected()) {
        conn->queue_message(msg);
    }
}

void MessengerServer::send_to_group(const std::vector<std::string>& user_ids, const WebSocketConnection::Message& msg) {
    for (const auto& user_id : user_ids) {
        send_to_user(user_id, msg);
    }
}

json MessengerServer::get_server_stats() const {
    json stats;
    stats["timestamp"] = std::time(nullptr);
    stats["uptime_seconds"] = 0;  // Would track this properly
    stats["total_connections"] = connection_pool_->get_total_connections();
    stats["online_users"] = connection_pool_->get_online_users().size();
    stats["redis_connected"] = redis_->is_connected();
    
    return stats;
}

json MessengerServer::get_user_status(const std::string& user_id) const {
    json status;
    
    auto conn = connection_pool_->find_by_user(user_id);
    if (conn) {
        status["online"] = true;
        status["user_id"] = user_id;
        status["last_activity"] = conn->get_last_activity();
        status["typing"] = conn->is_typing();
    } else {
        status["online"] = false;
        status["user_id"] = user_id;
        status["last_activity"] = 0;
    }
    
    return status;
}

// Private methods

void MessengerServer::subscribe_to_user_channel(const std::string& user_id) {
    std::string channel = "user:" + user_id;
    
    redis_->subscribe(channel, [this](const std::string& channel, const std::string& message) {
        this->redis_message_handler(channel, message);
    });
}

void MessengerServer::redis_message_handler(const std::string& channel, const std::string& message) {
    try {
        auto msg_json = json::parse(message);
        
        // Extract user_id from channel name
        std::string user_id = channel.substr(5);  // "user:" prefix is 5 chars
        
        send_to_user(user_id, WebSocketConnection::Message{
            .type = msg_json.value("type", "message"),
            .payload = msg_json,
            .timestamp = static_cast<uint64_t>(std::time(nullptr)),
            .requires_ack = true
        });
    } catch (const json::parse_error& e) {
        log("error", "Failed to parse Redis message: " + std::string(e.what()));
    }
}

bool MessengerServer::process_encrypted_message(const json& message, std::string& decrypted_content) {
    // Placeholder: Would verify HMAC and decrypt using session keys
    decrypted_content = message.value("content", "");
    return true;
}

std::string MessengerServer::encrypt_for_delivery(const std::string& content, const std::string& /*recipient_id*/) {
    // Placeholder: Would encrypt using recipient's session key
    return content;
}

bool MessengerServer::authenticate_user(const std::string& username, const std::string& /*password*/, 
                                       std::string& user_id, std::string& session_token) {
    // TODO: Implement proper authentication with database
    // This is a placeholder that accepts any user for demo
    
    user_id = username;
    session_token = "token_" + username + "_" + std::to_string(std::time(nullptr));
    
    return true;
}

bool MessengerServer::validate_session(const std::string& /*session_token*/, std::string& /*user_id*/) {
    // TODO: Implement session validation
    return false;
}

void MessengerServer::store_session(const std::string& user_id, const std::string& session_token) {
    redis_->set("session:" + session_token, user_id, 86400);  // TTL: 24 hours
}

void MessengerServer::mark_user_online(const std::string& user_id) {
    redis_->set("online:" + user_id, std::to_string(std::time(nullptr)), 300);  // TTL: 5 minutes
    
    // Publish user online event
    json online_event = {
        {"type", "user_online"},
        {"user_id", user_id},
        {"timestamp", std::time(nullptr)}
    };
    redis_->publish("user_status", online_event.dump());
}

void MessengerServer::mark_user_offline(const std::string& user_id) {
    redis_->del("online:" + user_id);
    
    // Publish user offline event
    json offline_event = {
        {"type", "user_offline"},
        {"user_id", user_id},
        {"last_seen", std::time(nullptr)}
    };
    redis_->publish("user_status", offline_event.dump());
}

void MessengerServer::on_redis_connected() {
    log("info", "Redis connection established");
}

void MessengerServer::on_redis_disconnected() {
    log("error", "Redis connection lost - attempting reconnect");
}

void MessengerServer::on_message_delivered(const std::string& message_id, const std::string& recipient_id) {
    log("info", "Message " + message_id + " delivered to " + recipient_id);
}

void MessengerServer::log(const std::string& level, const std::string& message) {
    std::cout << "[" << get_timestamp() << "] [" << level << "] " << message << std::endl;
}

std::string MessengerServer::get_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}
