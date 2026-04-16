#pragma once

#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>

#include <crow.h>
struct pg_conn; typedef pg_conn PGconn;

using json = nlohmann::json;

class RedisClient;

// ============================================================
// Thin DB wrapper (libpq)
// ============================================================
class PgDb {
public:
    explicit PgDb(const std::string& conn_url);
    ~PgDb();

    bool connected() const;

    // Returns rows as vector of json objects.
    // Throws std::runtime_error on query failure.
    std::vector<json> query(const std::string& sql,
                            const std::vector<std::string>& params = {});

    // Returns number of rows affected.
    int exec(const std::string& sql,
             const std::vector<std::string>& params = {});

private:
    PGconn* conn_ = nullptr;
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

    std::string authenticate_request(const std::string& auth_header, int& status_code) const;

private:
    Config cfg_;
    std::unique_ptr<PgDb> db_;
    std::unique_ptr<crow::SimpleApp> app_;
    std::unique_ptr<RedisClient> redis_;

    // ---- route handlers ----
    void register_routes();

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
