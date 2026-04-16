#include "websocket_connection.hpp"
#include <chrono>
#include <iostream>

// WebSocketConnection Implementation

WebSocketConnection::WebSocketConnection(int connection_id) 
    : connection_id_(connection_id) {
    created_at_ = std::chrono::system_clock::now().time_since_epoch().count();
    last_activity_ = created_at_;
}

void WebSocketConnection::set_user_id(const std::string& user_id) {
    user_id_ = user_id;
}

void WebSocketConnection::set_session_id(const std::string& session_id) {
    session_id_ = session_id;
}

std::string WebSocketConnection::get_user_id() const {
    return user_id_;
}

std::string WebSocketConnection::get_session_id() const {
    return session_id_;
}

void WebSocketConnection::mark_authenticated() {
    authenticated_ = true;
    update_activity();
}

bool WebSocketConnection::is_authenticated() const {
    return authenticated_;
}

void WebSocketConnection::mark_connected() {
    connected_ = true;
    update_activity();
}

void WebSocketConnection::mark_disconnected() {
    connected_ = false;
}

bool WebSocketConnection::is_connected() const {
    return connected_;
}

void WebSocketConnection::queue_message(const Message& msg) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    outgoing_queue_.push_back(msg);
}

bool WebSocketConnection::dequeue_message(Message& msg) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (outgoing_queue_.empty()) {
        return false;
    }
    msg = outgoing_queue_.front();
    outgoing_queue_.erase(outgoing_queue_.begin());
    return true;
}

size_t WebSocketConnection::pending_messages() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return outgoing_queue_.size();
}

void WebSocketConnection::update_activity() {
    last_activity_ = std::chrono::system_clock::now().time_since_epoch().count();
}

// ConnectionPool Implementation

std::shared_ptr<WebSocketConnection> ConnectionPool::create_connection(int connection_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (connections_.size() >= MAX_CONNECTIONS) {
        std::cerr << "❌ Connection pool full!" << std::endl;
        return nullptr;
    }
    
    auto conn = std::make_shared<WebSocketConnection>(connection_id);
    connections_[connection_id] = conn;
    return conn;
}

bool ConnectionPool::remove_connection(int connection_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = connections_.find(connection_id);
    if (it != connections_.end()) {
        std::string user_id = it->second->get_user_id();
        connections_.erase(it);
        
        if (!user_id.empty()) {
            user_to_connection_.erase(user_id);
        }
        
        return true;
    }
    
    return false;
}

std::shared_ptr<WebSocketConnection> ConnectionPool::get_connection(int connection_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = connections_.find(connection_id);
    if (it != connections_.end()) {
        return it->second;
    }
    
    return nullptr;
}

void ConnectionPool::register_user(const std::string& user_id, int connection_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    user_to_connection_[user_id] = connection_id;
}

std::shared_ptr<WebSocketConnection> ConnectionPool::find_by_user(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = user_to_connection_.find(user_id);
    if (it != user_to_connection_.end()) {
        return connections_[it->second];
    }
    
    return nullptr;
}

void ConnectionPool::broadcast_to_user(const std::string& user_id, const WebSocketConnection::Message& msg) {
    auto conn = find_by_user(user_id);
    if (conn && conn->is_connected()) {
        conn->queue_message(msg);
    }
}

void ConnectionPool::broadcast_except(int exclude_connection_id, const WebSocketConnection::Message& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& [conn_id, conn] : connections_) {
        if (conn_id != exclude_connection_id && conn->is_connected()) {
            conn->queue_message(msg);
        }
    }
}

size_t ConnectionPool::get_total_connections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

std::vector<std::string> ConnectionPool::get_online_users() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> users;
    for (const auto& [user_id, _] : user_to_connection_) {
        users.push_back(user_id);
    }
    
    return users;
}

void ConnectionPool::cleanup_inactive(uint64_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint64_t now = std::chrono::system_clock::now().time_since_epoch().count();
    std::vector<int> to_remove;
    
    for (auto& [conn_id, conn] : connections_) {
        uint64_t inactive_time = now - conn->get_last_activity();
        
        if (inactive_time > timeout_ms) {
            to_remove.push_back(conn_id);
        }
    }
    
    for (int conn_id : to_remove) {
        std::string user_id = connections_[conn_id]->get_user_id();
        connections_.erase(conn_id);
        
        if (!user_id.empty()) {
            user_to_connection_.erase(user_id);
        }
        
        std::cout << "🗑️  Removed inactive connection " << conn_id << std::endl;
    }
}
