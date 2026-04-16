#include "redis_client.hpp"
#include <hiredis/hiredis.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>

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
        return true;
    }

    int attempt = 0;
    while (attempt < config_.max_retries) {
        ++attempt;
        bool ok = true;

        for (auto& connection : pool_) {
            if (connection->context) {
                redisFree(connection->context);
                connection->context = nullptr;
            }

            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            connection->context = redisConnectWithTimeout(config_.host.c_str(), config_.port, tv);
            if (connection->context == nullptr || connection->context->err) {
                ok = false;
                if (connection->context) {
                    redisFree(connection->context);
                    connection->context = nullptr;
                }
                break;
            }

            redisReply* reply = (redisReply*)redisCommand(connection->context, "PING");
            if (reply == nullptr || reply->type != REDIS_REPLY_STATUS || strcmp(reply->str, "PONG") != 0) {
                ok = false;
                if (reply) freeReplyObject(reply);
                break;
            }
            freeReplyObject(reply);
        }

        if (ok) {
            running_ = true;
            subscriber_thread_ = std::thread(&RedisClient::subscriber_loop, this);
            std::cout << "Connected to Redis" << std::endl;
            return true;
        }

        disconnect();
        last_error_ = "Failed to connect to Redis (attempt " + std::to_string(attempt) + ")";
        if (error_callback_) error_callback_(last_error_);
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
    }

    last_error_ = "Failed to connect to Redis after " + std::to_string(config_.max_retries) + " attempts";
    if (error_callback_) error_callback_(last_error_);
    return false;
}

void RedisClient::disconnect() {
    running_ = false;
    
    if (subscriber_thread_.joinable()) {
        subscriber_thread_.join();
    }

    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (auto& connection : pool_) {
        if (connection->context) {
            redisFree(connection->context);
            connection->context = nullptr;
        }
    }
}

bool RedisClient::reconnect() {
    disconnect();
    return connect();
}

bool RedisClient::is_connected() const {
    return running_;
}

std::shared_ptr<RedisClient::Connection> RedisClient::acquire_connection() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (auto& conn : pool_) {
        if (!conn->in_use) {
            conn->in_use = true;
            return std::shared_ptr<Connection>(conn.get(), [this](Connection* c) {
                c->in_use = false;
            });
        }
    }
    return nullptr;
}

std::string RedisClient::send_command(Connection& conn, const std::string& cmd) {
    redisReply* reply = (redisReply*)redisCommand(conn.context, cmd.c_str());
    if (reply == nullptr && conn.context && conn.context->err) {
        last_error_ = "Redis command failed, trying reconnect";
        if (error_callback_) error_callback_(last_error_);
        if (reconnect()) {
            reply = (redisReply*)redisCommand(conn.context, cmd.c_str());
        }
    }
    if (reply == nullptr) {
        return "";
    }
    
    std::string result;
    switch (reply->type) {
        case REDIS_REPLY_STRING:
        case REDIS_REPLY_STATUS:
            result = reply->str ? reply->str : "";
            break;
        case REDIS_REPLY_INTEGER:
            result = std::to_string(reply->integer);
            break;
        case REDIS_REPLY_ARRAY:
            // For simplicity, return first element
            if (reply->elements > 0 && reply->element[0]->str) {
                result = reply->element[0]->str;
            }
            break;
        default:
            result = "";
    }
    
    freeReplyObject(reply);
    return result;
}

bool RedisClient::set(const std::string& key, const std::string& value, int ttl_seconds) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string cmd = "SET " + key + " " + value;
    if (ttl_seconds > 0) {
        cmd += " EX " + std::to_string(ttl_seconds);
    }
    
    std::string reply = send_command(*conn, cmd);
    return reply == "OK";
}

std::string RedisClient::get(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return "";
    
    return send_command(*conn, "GET " + key);
}

bool RedisClient::exists(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string reply = send_command(*conn, "EXISTS " + key);
    return reply == "1";
}

bool RedisClient::del(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string reply = send_command(*conn, "DEL " + key);
    return reply != "0";
}

bool RedisClient::expire(const std::string& key, int seconds) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string reply = send_command(*conn, "EXPIRE " + key + " " + std::to_string(seconds));
    return reply == "1";
}

bool RedisClient::lpush(const std::string& key, const std::vector<std::string>& values) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string cmd = "LPUSH " + key;
    for (const auto& val : values) {
        cmd += " " + val;
    }
    
    send_command(*conn, cmd);
    return true;
}

std::vector<std::string> RedisClient::lrange(const std::string& key, int start, int stop) {
    auto conn = acquire_connection();
    if (!conn) return {};
    
    redisReply* reply = (redisReply*)redisCommand(conn->context, "LRANGE %s %d %d", key.c_str(), start, stop);
    if (reply == nullptr && conn->context && conn->context->err) {
        last_error_ = "Redis LRANGE failed, trying reconnect";
        if (error_callback_) error_callback_(last_error_);
        if (reconnect()) {
            reply = (redisReply*)redisCommand(conn->context, "LRANGE %s %d %d", key.c_str(), start, stop);
        }
    }

    std::vector<std::string> result;
    
    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < reply->elements; ++i) {
            if (reply->element[i]->str) {
                result.push_back(reply->element[i]->str);
            }
        }
    }
    
    if (reply) freeReplyObject(reply);
    return result;
}

int RedisClient::llen(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return 0;
    
    std::string reply = send_command(*conn, "LLEN " + key);
    return std::stoi(reply);
}

bool RedisClient::sadd(const std::string& key, const std::vector<std::string>& members) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string cmd = "SADD " + key;
    for (const auto& member : members) {
        cmd += " " + member;
    }
    
    send_command(*conn, cmd);
    return true;
}

std::vector<std::string> RedisClient::smembers(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return {};
    
    redisReply* reply = (redisReply*)redisCommand(conn->context, "SMEMBERS %s", key.c_str());
    if (reply == nullptr && conn->context && conn->context->err) {
        last_error_ = "Redis SMEMBERS failed, trying reconnect";
        if (error_callback_) error_callback_(last_error_);
        if (reconnect()) {
            reply = (redisReply*)redisCommand(conn->context, "SMEMBERS %s", key.c_str());
        }
    }

    std::vector<std::string> result;
    
    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < reply->elements; ++i) {
            if (reply->element[i]->str) {
                result.push_back(reply->element[i]->str);
            }
        }
    }
    
    if (reply) freeReplyObject(reply);
    return result;
}

bool RedisClient::sismember(const std::string& key, const std::string& member) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string reply = send_command(*conn, "SISMEMBER " + key + " " + member);
    return reply == "1";
}

bool RedisClient::srem(const std::string& key, const std::string& member) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string reply = send_command(*conn, "SREM " + key + " " + member);
    return reply != "0";
}

bool RedisClient::hset(const std::string& key, const std::string& field, const std::string& value) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string reply = send_command(*conn, "HSET " + key + " " + field + " " + value);
    return reply != "0";
}

std::string RedisClient::hget(const std::string& key, const std::string& field) {
    auto conn = acquire_connection();
    if (!conn) return "";
    
    return send_command(*conn, "HGET " + key + " " + field);
}

bool RedisClient::hdel(const std::string& key, const std::string& field) {
    auto conn = acquire_connection();
    if (!conn) return false;
    
    std::string reply = send_command(*conn, "HDEL " + key + " " + field);
    return reply != "0";
}

std::vector<std::pair<std::string, std::string>> RedisClient::hgetall(const std::string& key) {
    auto conn = acquire_connection();
    if (!conn) return {};
    
    redisReply* reply = (redisReply*)redisCommand(conn->context, "HGETALL %s", key.c_str());
    if (reply == nullptr && conn->context && conn->context->err) {
        last_error_ = "Redis HGETALL failed, trying reconnect";
        if (error_callback_) error_callback_(last_error_);
        if (reconnect()) {
            reply = (redisReply*)redisCommand(conn->context, "HGETALL %s", key.c_str());
        }
    }

    std::vector<std::pair<std::string, std::string>> result;
    
    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < reply->elements; i += 2) {
            if (reply->element[i]->str && reply->element[i+1]->str) {
                result.emplace_back(reply->element[i]->str, reply->element[i+1]->str);
            }
        }
    }
    
    if (reply) freeReplyObject(reply);
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
    
    send_command(*conn, "PUBLISH " + channel + " " + message);
    return true;
}

int RedisClient::get_subscription_count(const std::string& channel) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    return subscribers_.count(channel);
}

bool RedisClient::multi_start() {
    // Not implemented
    return false;
}

bool RedisClient::queue_command(const std::string& /*cmd*/) {
    // Not implemented
    return false;
}

std::vector<std::string> RedisClient::exec() {
    // Not implemented
    return {};
}

void RedisClient::discard() {
    // Not implemented
}

std::string RedisClient::ping() {
    auto conn = acquire_connection();
    if (!conn) return "";
    
    return send_command(*conn, "PING");
}

std::string RedisClient::info(const std::string& section) {
    auto conn = acquire_connection();
    if (!conn) return "";
    
    return send_command(*conn, "INFO " + section);
}

void RedisClient::set_error_callback(ErrorCallback callback) {
    error_callback_ = callback;
}

std::string RedisClient::get_last_error() const {
    return last_error_;
}

void RedisClient::subscriber_loop() {
    // For simplicity, subscribers are not implemented in this basic version
    // In a real implementation, you'd have a separate connection for subscriptions
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}