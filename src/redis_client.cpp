#include "redis_client.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

RedisClient::RedisClient(const Config& config) 
    : config_(config), running_(false) {
    
    // Initialize connection pool
    for (int i = 0; i < config_.pool_size; ++i) {
        pool_.push_back(std::make_unique<Connection>());
    }
}

RedisClient::~RedisClient() {
    disconnect();
}

bool RedisClient::connect() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    if (running_) {
        return true;  // Already connected
    }

    // Try to connect to Redis
    for (auto& connection : pool_) {
        connection->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        
        if (connection->socket_fd < 0) {
            last_error_ = "Failed to create socket";
            if (error_callback_) error_callback_(last_error_);
            return false;
        }

        // Set non-blocking mode
        int flags = fcntl(connection->socket_fd, F_GETFL, 0);
        fcntl(connection->socket_fd, F_SETFL, flags | O_NONBLOCK);

        // Set socket timeout
        struct timeval tv;
        tv.tv_sec = config_.timeout_ms / 1000;
        tv.tv_usec = (config_.timeout_ms % 1000) * 1000;
        setsockopt(connection->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(connection->socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // Connect to Redis
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr);

        if (::connect(connection->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            if (errno != EINPROGRESS) {
                last_error_ = "Failed to connect to Redis at " + config_.host + ":" + std::to_string(config_.port);
                if (error_callback_) error_callback_(last_error_);
                return false;
            }
        }

        // Test connection with PING
        std::string pong = ping();
        if (pong != "PONG") {
            last_error_ = "Redis PING failed";
            if (error_callback_) error_callback_(last_error_);
            return false;
        }
    }

    running_ = true;
    
    // Start subscriber thread for Pub/Sub
    subscriber_thread_ = std::thread(&RedisClient::subscriber_loop, this);
    
    std::cout << "✅ Connected to Redis at " << config_.host << ":" << config_.port << std::endl;
    return true;
}

void RedisClient::disconnect() {
    running_ = false;
    
    if (subscriber_thread_.joinable()) {
        subscriber_thread_.join();
    }

    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (auto& connection : pool_) {
        if (connection->socket_fd >= 0) {
            close(connection->socket_fd);
            connection->socket_fd = -1;
        }
    }
    
    std::cout << "🔌 Disconnected from Redis" << std::endl;
}

bool RedisClient::is_connected() const {
    return running_;
}

bool RedisClient::set(const std::string& key, const std::string& value, int ttl_seconds) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::ostringstream oss;
    oss << "SET " << key << " \"" << value << "\"";
    if (ttl_seconds > 0) {
        oss << " EX " << ttl_seconds;
    }
    
    std::string response = send_command(*conn, oss.str());
    release_connection(std::move(conn));
    
    return response == "+OK";
}

std::string RedisClient::get(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return "";

    std::string response = send_command(*conn, "GET " + key);
    release_connection(std::move(conn));
    
    // Parse RESP protocol: $<length>\r\n<data>\r\n
    if (response.find("$-1") == 0) {
        return "";  // Null bulk string (key not found)
    }
    
    // Extract value between $ marker and \r\n
    size_t pos = response.find('\n');
    if (pos != std::string::npos) {
        return response.substr(pos + 1);
    }
    
    return response;
}

bool RedisClient::exists(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::string response = send_command(*conn, "EXISTS " + key);
    release_connection(std::move(conn));
    
    return response.find(":1") != std::string::npos;
}

bool RedisClient::del(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::string response = send_command(*conn, "DEL " + key);
    release_connection(std::move(conn));
    
    return response.find(":1") != std::string::npos;
}

bool RedisClient::expire(const std::string& key, int seconds) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::string response = send_command(*conn, "EXPIRE " + key + " " + std::to_string(seconds));
    release_connection(std::move(conn));
    
    return response.find(":1") != std::string::npos;
}

bool RedisClient::lpush(const std::string& key, const std::vector<std::string>& values) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::ostringstream oss;
    oss << "LPUSH " << key;
    for (const auto& v : values) {
        oss << " \"" << v << "\"";
    }
    
    std::string response = send_command(*conn, oss.str());
    release_connection(std::move(conn));
    
    return response.find(":") != std::string::npos;
}

std::vector<std::string> RedisClient::lrange(const std::string& key, int start, int stop) {
    auto conn = acquire_connection();
    if (!conn) return {};

    std::string cmd = "LRANGE " + key + " " + std::to_string(start) + " " + std::to_string(stop);
    std::string response = send_command(*conn, cmd);
    release_connection(std::move(conn));
    
    // Parse RESP array: *<count>\r\n followed by bulk strings
    std::vector<std::string> result;
    // Simplified parsing (full implementation would use proper RESP parser)
    return result;
}

int RedisClient::llen(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return 0;

    std::string response = send_command(*conn, "LLEN " + key);
    release_connection(std::move(conn));
    
    // Parse integer response: :<number>\r\n
    size_t pos = response.find(':');
    if (pos != std::string::npos) {
        return std::stoi(response.substr(pos + 1));
    }
    
    return 0;
}

bool RedisClient::sadd(const std::string& key, const std::vector<std::string>& members) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::ostringstream oss;
    oss << "SADD " << key;
    for (const auto& m : members) {
        oss << " \"" << m << "\"";
    }
    
    std::string response = send_command(*conn, oss.str());
    release_connection(std::move(conn));
    
    return response.find(":") != std::string::npos;
}

std::vector<std::string> RedisClient::smembers(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return {};

    std::string response = send_command(*conn, "SMEMBERS " + key);
    release_connection(std::move(conn));
    
    std::vector<std::string> result;
    // Simplified parsing
    return result;
}

bool RedisClient::sismember(const std::string& key, const std::string& member) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::string response = send_command(*conn, "SISMEMBER " + key + " \"" + member + "\"");
    release_connection(std::move(conn));
    
    return response.find(":1") != std::string::npos;
}

bool RedisClient::srem(const std::string& key, const std::string& member) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::string response = send_command(*conn, "SREM " + key + " \"" + member + "\"");
    release_connection(std::move(conn));
    
    return response.find(":1") != std::string::npos;
}

bool RedisClient::hset(const std::string& key, const std::string& field, const std::string& value) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::string cmd = "HSET " + key + " \"" + field + "\" \"" + value + "\"";
    std::string response = send_command(*conn, cmd);
    release_connection(std::move(conn));
    
    return response.find(":") != std::string::npos;
}

std::string RedisClient::hget(const std::string& key, const std::string& field) {
    auto conn = acquire_connection();
    if (!conn) return "";

    std::string response = send_command(*conn, "HGET " + key + " \"" + field + "\"");
    release_connection(std::move(conn));
    
    return response;
}

bool RedisClient::hdel(const std::string& key, const std::string& field) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::string response = send_command(*conn, "HDEL " + key + " \"" + field + "\"");
    release_connection(std::move(conn));
    
    return response.find(":1") != std::string::npos;
}

std::vector<std::pair<std::string, std::string>> RedisClient::hgetall(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return {};

    std::string response = send_command(*conn, "HGETALL " + key);
    release_connection(std::move(conn));
    
    std::vector<std::pair<std::string, std::string>> result;
    // Simplified parsing
    return result;
}

bool RedisClient::subscribe(const std::string& channel, SubscriberCallback callback) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_[channel] = callback;
    return true;
}

bool RedisClient::unsubscribe(const std::string& channel) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(channel);
    return true;
}

bool RedisClient::publish(const std::string& channel, const std::string& message) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::string cmd = "PUBLISH " + channel + " \"" + message + "\"";
    std::string response = send_command(*conn, cmd);
    release_connection(std::move(conn));
    
    return response.find(":") != std::string::npos;
}

int RedisClient::get_subscription_count(const std::string& channel) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    auto it = subscribers_.find(channel);
    return it != subscribers_.end() ? 1 : 0;
}

bool RedisClient::multi_start() {
    // Transaction support would require dedicated connection
    return true;
}

bool RedisClient::queue_command(const std::string& cmd) {
    // Would queue commands in transaction
    return true;
}

std::vector<std::string> RedisClient::exec() {
    // Execute queued commands
    return {};
}

void RedisClient::discard() {
    // Discard queued commands
}

std::string RedisClient::ping() {
    auto conn = acquire_connection();
    if (!conn) return "";

    std::string response = send_command(*conn, "PING");
    release_connection(std::move(conn));
    
    return response.find("PONG") != std::string::npos ? "PONG" : "";
}

std::string RedisClient::info(const std::string& section) {
    auto conn = acquire_connection();
    if (!conn) return "";

    std::string cmd = "INFO " + section;
    std::string response = send_command(*conn, cmd);
    release_connection(std::move(conn));
    
    return response;
}

void RedisClient::set_error_callback(ErrorCallback callback) {
    error_callback_ = callback;
}

std::string RedisClient::get_last_error() const {
    return last_error_;
}

// Private methods

std::unique_ptr<RedisClient::Connection> RedisClient::acquire_connection() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (pool_.empty()) {
        return nullptr;
    }

    for (auto& conn : pool_) {
        if (!conn->in_use) {
            conn->in_use = true;
            return std::move(conn);
        }
    }

    // All connections busy, create temporary one
    return std::make_unique<Connection>();
}

void RedisClient::release_connection(std::unique_ptr<Connection> conn) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (conn) {
        conn->in_use = false;
    }
}

std::string RedisClient::send_command(Connection& conn, const std::string& cmd) {
    if (conn.socket_fd < 0) return "";

    // Send RESP command
    std::string resp_cmd = "*1\r\n$" + std::to_string(cmd.length()) + "\r\n" + cmd + "\r\n";
    
    if (send(conn.socket_fd, resp_cmd.c_str(), resp_cmd.length(), 0) < 0) {
        return "";
    }

    // Receive response
    char buffer[4096];
    ssize_t n = recv(conn.socket_fd, buffer, sizeof(buffer) - 1, 0);
    if (n < 0) {
        return "";
    }

    buffer[n] = '\0';
    return std::string(buffer);
}

void RedisClient::subscriber_loop() {
    // Simplified subscriber loop
    // Full implementation would use SUBSCRIBE Redis command
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void RedisClient::handle_reconnect() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100 * (++reconnect_attempts_)));
    connect();
}
