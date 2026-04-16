#pragma once

#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <nlohmann/json.hpp>
#include <libpq-fe.h>

#include <crow.h>
#include "messenger_server.hpp"

using json = nlohmann::json;

class RedisClient;

// ============================================================
// Thin DB wrapper (libpq)
// ============================================================
class PgDb {
public:
    PGconn* conn_ = nullptr;
    std::mutex mutex_;

    explicit PgDb(const std::string& conn_url) {
        // Add connect_timeout so constructor never hangs Railway healthcheck
        std::string url = conn_url;
        if (url.find("connect_timeout") == std::string::npos) {
            url += (url.find('?') == std::string::npos ? "?" : "&");
            url += "connect_timeout=5";
        }
        conn_ = PQconnectdb(url.c_str());
        if (PQstatus(conn_) != CONNECTION_OK) {
            std::cerr << "[DB] Connection failed: " << PQerrorMessage(conn_) << std::endl;
            PQfinish(conn_);
            conn_ = nullptr;
        } else {
            std::cout << "[DB] Connected to PostgreSQL" << std::endl;
        }
    }

    ~PgDb() {
        if (conn_) PQfinish(conn_);
    }

    bool connected() const {
        return conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK;
    }

    // Returns rows as vector of json objects.
    // Throws std::runtime_error on query failure.
    std::vector<json> query(const std::string& sql,
                            const std::vector<std::string>& params = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!conn_) throw std::runtime_error("No database connection");

        std::vector<const char*> vals;
        for (auto& p : params) vals.push_back(p.c_str());

        PGresult* res = PQexecParams(conn_, sql.c_str(),
            (int)params.size(), nullptr,
            params.empty() ? nullptr : vals.data(),
            nullptr, nullptr, 0);

        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::string err = PQerrorMessage(conn_);
            PQclear(res);
            throw std::runtime_error("Query failed: " + err);
        }

        std::vector<json> rows;
        int nrows = PQntuples(res);
        int ncols = PQnfields(res);

        for (int r = 0; r < nrows; r++) {
            json row;
            for (int c = 0; c < ncols; c++) {
                const char* name = PQfname(res, c);
                if (PQgetisnull(res, r, c)) {
                    row[name] = nullptr;
                } else {
                    row[name] = std::string(PQgetvalue(res, r, c));
                }
            }
            rows.push_back(row);
        }
        PQclear(res);
        return rows;
    }

    // Returns number of rows affected.
    int exec(const std::string& sql,
             const std::vector<std::string>& params = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!conn_) throw std::runtime_error("No database connection");

        std::vector<const char*> vals;
        for (auto& p : params) vals.push_back(p.c_str());

        PGresult* res = PQexecParams(conn_, sql.c_str(),
            (int)params.size(), nullptr,
            params.empty() ? nullptr : vals.data(),
            nullptr, nullptr, 0);

        ExecStatusType st = PQresultStatus(res);
        if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
            std::string err = PQerrorMessage(conn_);
            PQclear(res);
            throw std::runtime_error("Exec failed: " + err);
        }
        int affected = 0;
        const char* tag = PQcmdTuples(res);
        if (tag && tag[0]) {
            std::string s(tag);
            auto pos = s.rfind(' ');
            try {
                affected = std::stoi(pos != std::string::npos ? s.substr(pos+1) : s);
            } catch (...) { affected = 0; }
        }
        PQclear(res);
        return affected;
    }
};

// ============================================================
// JWT helpers  (HS256 via libsodium / manual)
// ============================================================
namespace JwtHelper {
    std::string create(const std::string& user_id, const std::string& secret);
    // Returns user_id on success, empty string on failure
    std::string verify(const std::string& token, const std::string& secret);
}

// ============================================================
// OTP helpers (stored in Redis)
// ============================================================
namespace OtpHelper {
    std::string generate();   // 6-digit string
    std::string hash(const std::string& code);
}

// ============================================================
// HTTP API Server
// ============================================================
class HttpApi {
public:
    struct Config {
        uint16_t    port        = 8080;
        std::string db_url;
        std::string redis_host  = "localhost";
        int         redis_port  = 6379;
        std::string jwt_secret  = "change_me_in_prod";
        std::string smtp_host;       // for e-mail OTP (optional)
        int         smtp_port   = 587;
        std::string smtp_user;
        std::string smtp_pass;
        bool        log_requests = true;
    };

    explicit HttpApi(const Config& cfg);
    ~HttpApi();

    // Blocks until stopped
    void run();
    void stop();

    // Attach the MessengerServer so /ws can delegate to it
    void set_messenger(MessengerServer* ms) { messenger_ = ms; }

    std::string authenticate_request(const std::string& auth_header, int& status_code) const;

private:
    void run_migrations(); // called from run() in background thread
    Config cfg_;
    std::unique_ptr<PgDb> db_;
    std::unique_ptr<crow::SimpleApp> app_;
    std::unique_ptr<RedisClient> redis_;
    MessengerServer* messenger_ = nullptr;  // not owned

    // ---- route handlers ----
    void register_routes();

    // WebSocket
    void route_websocket();          // GET /ws (upgrade)

    // Auth
    void route_send_otp();       // POST /api/auth/send-otp
    void route_verify_otp();     // POST /api/auth/verify-otp
    void route_register();       // POST /api/auth/register
    void route_get_me();         // GET  /api/auth/me
    void route_update_me();      // PATCH /api/auth/me
    void route_get_avatar();     // POST /api/auth/avatar (stub)
    void route_search_users();   // GET  /api/auth/users/search
    void route_get_user();       // GET  /api/auth/users/:id

    // Chats
    void route_get_chats();          // GET  /api/chats
    void route_open_direct();        // POST /api/chats/direct
    void route_create_group();       // POST /api/chats/group
    void route_get_chat_info();      // GET  /api/chats/:id
    void route_get_messages();       // GET  /api/chats/:id/messages
    void route_get_members();        // GET  /api/chats/:id/members
    void route_add_member();         // POST /api/chats/:id/members
    void route_remove_member();      // DELETE /api/chats/:id/members/:uid
    void route_search_messages();    // GET  /api/chats/:id/search
    void route_pin_message();        // POST /api/chats/:id/messages/:mid/pin
    void route_react_message();      // POST /api/chats/:id/messages/:mid/react
    void route_mute_chat();          // POST /api/chats/:id/mute
    void route_chat_search_users();  // GET  /api/chats/users/search

    // Contacts
    void route_get_contacts();       // GET  /api/contacts
    void route_add_contact();        // POST /api/contacts
    void route_remove_contact();     // DELETE /api/contacts/:id
    void route_check_contact();      // GET  /api/contacts/check/:id
    void route_find_by_phone();      // GET  /api/contacts/find-by-phone

    // Channels
    void route_explore_channels();   // GET  /api/channels/explore
    void route_my_channels();        // GET  /api/channels/my
    void route_create_channel();     // POST /api/channels
    void route_get_channel();        // GET  /api/channels/:username
    void route_subscribe_channel();  // POST /api/channels/:id/subscribe
    void route_get_channel_posts();  // GET  /api/channels/:id/posts
    void route_create_post();        // POST /api/channels/:id/posts
    void route_delete_post();        // DELETE /api/channels/:id/posts/:pid

    // Health
    void route_health();             // GET  /health

    // ---- helpers ----
    std::string extract_token(const std::string& auth_header) const;
    std::string authenticate_request(const std::string& auth_header) const;
    bool redis_available() const;
    void store_redis_session(const std::string& session_token, const std::string& user_id);
    bool send_otp_email(const std::string& email, const std::string& code);
    std::string hash_password(const std::string& password);
    bool verify_password(const std::string& password, const std::string& hash);
};
