#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @class WebSocketConnection
 * @brief Represents a single WebSocket client connection
 * 
 * Manages:
 * - Client metadata (user_id, session_id)
 * - Incoming/outgoing message queues
 * - Connection state
 */
class WebSocketConnection {
public:
    struct Message {
        std::string type;           // "message", "typing", "call", "notification"
        json payload;
        uint64_t timestamp = 0;
        bool requires_ack = false;
    };

    explicit WebSocketConnection(int connection_id);
    ~WebSocketConnection() = default;

    // Connection Management
    void set_user_id(const std::string& user_id);
    void set_session_id(const std::string& session_id);
    std::string get_user_id() const;
    std::string get_session_id() const;
    int get_connection_id() const { return connection_id_; }

    // State Management
    void mark_authenticated();
    bool is_authenticated() const;
    void mark_connected();
    void mark_disconnected();
    bool is_connected() const;
    
    // Message Handling
    void queue_message(const Message& msg);
    bool dequeue_message(Message& msg);
    size_t pending_messages() const;

    // Metadata
    uint64_t get_created_at() const { return created_at_; }
    uint64_t get_last_activity() const { return last_activity_; }
    void update_activity();

    // Typing indicator
    void set_typing(bool is_typing) { is_typing_ = is_typing; }
    bool is_typing() const { return is_typing_; }

private:
    int connection_id_;
    std::string user_id_;
    std::string session_id_;
    bool authenticated_ = false;
    bool connected_ = false;
    bool is_typing_ = false;
    
    uint64_t created_at_;
    uint64_t last_activity_;
    
    std::vector<Message> outgoing_queue_;
    mutable std::mutex queue_mutex_;
};

/**
 * @class ConnectionPool
 * @brief Manages all active WebSocket connections
 */
class ConnectionPool {
public:
    static constexpr size_t MAX_CONNECTIONS = 100000;

    ConnectionPool() = default;
    ~ConnectionPool() = default;

    std::shared_ptr<WebSocketConnection> create_connection(int connection_id);
    bool remove_connection(int connection_id);
    std::shared_ptr<WebSocketConnection> get_connection(int connection_id);
    std::shared_ptr<WebSocketConnection> find_by_user(const std::string& user_id);
    
    // Broadcast operations
    void broadcast_to_user(const std::string& user_id, const WebSocketConnection::Message& msg);
    void broadcast_except(int exclude_connection_id, const WebSocketConnection::Message& msg);
    
    size_t get_total_connections() const;
    std::vector<std::string> get_online_users() const;

    // Cleanup
    void cleanup_inactive(uint64_t timeout_ms);

private:
    std::map<int, std::shared_ptr<WebSocketConnection>> connections_;
    std::map<std::string, int> user_to_connection_;
    mutable std::mutex mutex_;
};
