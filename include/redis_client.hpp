#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <map>

/**
 * @class RedisClient
 * @brief High-performance Redis client for pub/sub and caching
 * 
 * Features:
 * - Connection pooling
 * - Pub/Sub with async subscribers
 * - Exponential backoff reconnection
 * - Thread-safe operations
 */
class RedisClient {
public:
    using SubscriberCallback = std::function<void(const std::string& channel, const std::string& message)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    /**
     * @struct Config
     * Redis connection configuration
     */
    struct Config {
        std::string host = "localhost";
        int port = 6379;
        int pool_size = 32;
        int timeout_ms = 5000;
        int max_retries = 3;
        bool use_sentinel = false;
        std::vector<std::string> sentinel_addresses;
    };

    explicit RedisClient(const Config& config);
    ~RedisClient();

    // Connection Management
    bool connect();
    void disconnect();
    bool reconnect();
    bool is_connected() const;

    // Basic Operations
    bool set(const std::string& key, const std::string& value, int ttl_seconds = 0);
    std::string get(const std::string& key);
    bool exists(const std::string& key);
    bool del(const std::string& key);
    bool expire(const std::string& key, int seconds);

    // List Operations (for message queues)
    bool lpush(const std::string& key, const std::vector<std::string>& values);
    std::vector<std::string> lrange(const std::string& key, int start, int stop);
    int llen(const std::string& key);

    // Set Operations (for managing user groups)
    bool sadd(const std::string& key, const std::vector<std::string>& members);
    std::vector<std::string> smembers(const std::string& key);
    bool sismember(const std::string& key, const std::string& member);
    bool srem(const std::string& key, const std::string& member);

    // Hash Operations (for storing structured data)
    bool hset(const std::string& key, const std::string& field, const std::string& value);
    std::string hget(const std::string& key, const std::string& field);
    bool hdel(const std::string& key, const std::string& field);
    std::vector<std::pair<std::string, std::string>> hgetall(const std::string& key);

    // Pub/Sub
    bool subscribe(const std::string& channel, SubscriberCallback callback);
    bool unsubscribe(const std::string& channel);
    bool publish(const std::string& channel, const std::string& message);
    int get_subscription_count(const std::string& channel);

    // Transaction
    bool multi_start();
    bool queue_command(const std::string& cmd);
    std::vector<std::string> exec();
    void discard();

    // Server Info
    std::string ping();
    std::string info(const std::string& section = "all");

    // Error handling
    void set_error_callback(ErrorCallback callback);
    std::string get_last_error() const;

private:
    struct Connection {
        int socket_fd = -1;
        std::mutex mutex;
        bool in_use = false;
        std::chrono::system_clock::time_point created_at;
    };

    Config config_;
    std::vector<std::unique_ptr<Connection>> pool_;
    std::mutex pool_mutex_;
    std::mutex subscribers_mutex_;
    
    std::map<std::string, SubscriberCallback> subscribers_;
    std::thread subscriber_thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> reconnect_attempts_{0};

    ErrorCallback error_callback_;
    std::string last_error_;

    // Helper methods
    std::unique_ptr<Connection> acquire_connection();
    void release_connection(std::unique_ptr<Connection> conn);
    std::string send_command(Connection& conn, const std::string& cmd);
    void subscriber_loop();
    void handle_reconnect();
    std::string create_command(const std::vector<std::string>& args);
};
