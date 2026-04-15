/**
 * http_api.cpp
 * Full REST API implementation using Crow (header-only) + libpq.
 *
 * Endpoints implemented:
 *   POST /api/auth/send-otp
 *   POST /api/auth/verify-otp
 *   POST /api/auth/register
 *   GET  /api/auth/me
 *   PATCH /api/auth/me
 *   GET  /api/auth/users/search
 *   GET  /api/auth/users/:id
 *   GET  /api/chats
 *   POST /api/chats/direct
 *   POST /api/chats/group
 *   GET  /api/chats/:id
 *   GET  /api/chats/:id/messages
 *   GET  /api/chats/:id/members
 *   POST /api/chats/:id/members
 *   DELETE /api/chats/:id/members/:uid
 *   GET  /api/chats/:id/search
 *   POST /api/chats/:id/messages/:mid/pin
 *   POST /api/chats/:id/messages/:mid/react
 *   POST /api/chats/:id/mute
 *   GET  /api/chats/users/search
 *   GET  /api/contacts
 *   POST /api/contacts
 *   DELETE /api/contacts/:id
 *   GET  /api/contacts/check/:id
 *   GET  /api/contacts/find-by-phone
 *   GET  /api/channels/explore
 *   GET  /api/channels/my
 *   POST /api/channels
 *   GET  /api/channels/:username
 *   POST /api/channels/:id/subscribe
 *   GET  /api/channels/:id/posts
 *   POST /api/channels/:id/posts
 *   DELETE /api/channels/:id/posts/:pid
 *   GET  /health
 */

#define CROW_MAIN
#include <crow.h>                   // Crow single-header
#include <libpq-fe.h>               // PostgreSQL
#include <sodium.h>                 // libsodium (hashing + HMAC)

#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <regex>
#include <algorithm>

#include "http_api.hpp"

using json = nlohmann::json;

// ============================================================
// Macros / helpers
// ============================================================

#define JSON_OK(body)    crow::response(200, (body).dump())
#define JSON_CREATED(b)  crow::response(201, (b).dump())
#define JSON_ERR(code, msg) ([&]{ \
    crow::response r(code, json{{"error",(msg)}}.dump()); \
    r.add_header("Content-Type","application/json"); \
    return r; }())

static crow::response json_resp(int status, const json& body) {
    crow::response r(status, body.dump());
    r.add_header("Content-Type", "application/json");
    return r;
}

// ============================================================
// PgDb — thin libpq wrapper
// ============================================================

PgDb::PgDb(const std::string& conn_url) {
    conn_ = PQconnectdb(conn_url.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        std::cerr << "[DB] Connection failed: " << PQerrorMessage(conn_) << std::endl;
        PQfinish(conn_);
        conn_ = nullptr;
    } else {
        std::cout << "[DB] Connected to PostgreSQL" << std::endl;
    }
}

PgDb::~PgDb() {
    if (conn_) PQfinish(conn_);
}

bool PgDb::connected() const {
    return conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK;
}

std::vector<json> PgDb::query(const std::string& sql,
                               const std::vector<std::string>& params) {
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

int PgDb::exec(const std::string& sql,
                const std::vector<std::string>& params) {
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
    if (tag && tag[0]) affected = std::stoi(tag);
    PQclear(res);
    return affected;
}

// ============================================================
// JWT helpers — HS256 via libsodium HMAC-SHA256
// ============================================================

static std::string base64url_encode(const unsigned char* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned char b0 = data[i];
        unsigned char b1 = (i+1 < len) ? data[i+1] : 0;
        unsigned char b2 = (i+2 < len) ? data[i+2] : 0;
        out += tbl[b0 >> 2];
        out += tbl[((b0 & 3) << 4) | (b1 >> 4)];
        out += (i+1 < len) ? tbl[((b1 & 0xf) << 2) | (b2 >> 6)] : '=';
        out += (i+2 < len) ? tbl[b2 & 0x3f] : '=';
    }
    // URL-safe
    for (char& c : out) { if (c=='+') c='-'; if (c=='/') c='_'; }
    out.erase(std::remove(out.begin(), out.end(), '='), out.end());
    return out;
}

static std::string base64url_decode_str(const std::string& in) {
    std::string s = in;
    for (char& c : s) { if (c=='-') c='+'; if (c=='_') c='/'; }
    while (s.size() % 4) s += '=';
    std::string out;
    out.reserve(s.size() * 3 / 4);
    for (size_t i = 0; i < s.size(); i += 4) {
        auto val = [](char c) -> int {
            if (c>='A'&&c<='Z') return c-'A';
            if (c>='a'&&c<='z') return c-'a'+26;
            if (c>='0'&&c<='9') return c-'0'+52;
            if (c=='+') return 62; if (c=='/') return 63;
            return -1;
        };
        int v0=val(s[i]), v1=val(s[i+1]);
        int v2=(s[i+2]!='=')?val(s[i+2]):-1;
        int v3=(s[i+3]!='=')?val(s[i+3]):-1;
        if (v0<0||v1<0) break;
        out += (char)((v0<<2)|(v1>>4));
        if (v2>=0) out += (char)(((v1&0xf)<<4)|(v2>>2));
        if (v3>=0) out += (char)(((v2&3)<<6)|v3);
    }
    return out;
}

std::string JwtHelper::create(const std::string& user_id,
                               const std::string& secret) {
    // header.payload
    auto now = std::chrono::system_clock::now();
    long long iat = std::chrono::duration_cast<std::chrono::seconds>(
                        now.time_since_epoch()).count();
    long long exp = iat + 30 * 24 * 3600; // 30 days

    std::string header_json = R"({"alg":"HS256","typ":"JWT"})";
    json payload = {{"sub", user_id}, {"iat", iat}, {"exp", exp}};
    std::string payload_json = payload.dump();

    std::string hp = base64url_encode(
                         (const unsigned char*)header_json.data(),
                         header_json.size()) +
                     "." +
                     base64url_encode(
                         (const unsigned char*)payload_json.data(),
                         payload_json.size());

    unsigned char sig[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state,
        (const unsigned char*)secret.data(), secret.size());
    crypto_auth_hmacsha256_update(&state,
        (const unsigned char*)hp.data(), hp.size());
    crypto_auth_hmacsha256_final(&state, sig);

    return hp + "." + base64url_encode(sig, sizeof(sig));
}

std::string JwtHelper::verify(const std::string& token,
                               const std::string& secret) {
    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos)
        return "";

    std::string hp = token.substr(0, dot2);
    std::string sig_b64 = token.substr(dot2 + 1);

    // Verify HMAC
    unsigned char expected[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state,
        (const unsigned char*)secret.data(), secret.size());
    crypto_auth_hmacsha256_update(&state,
        (const unsigned char*)hp.data(), hp.size());
    crypto_auth_hmacsha256_final(&state, expected);

    std::string sig_raw = base64url_decode_str(sig_b64);
    if (sig_raw.size() != crypto_auth_hmacsha256_BYTES) return "";
    if (sodium_memcmp(expected,
                      (const unsigned char*)sig_raw.data(),
                      crypto_auth_hmacsha256_BYTES) != 0)
        return "";

    // Decode payload
    std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string payload_str = base64url_decode_str(payload_b64);
    try {
        auto pl = json::parse(payload_str);
        long long exp = pl.value("exp", 0LL);
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        if (exp < now) return ""; // expired
        return pl.value("sub", "");
    } catch (...) {
        return "";
    }
}

// ============================================================
// OTP helpers
// ============================================================

std::string OtpHelper::generate() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    return std::to_string(dis(gen));
}

std::string OtpHelper::hash(const std::string& code) {
    unsigned char hash[crypto_generichash_BYTES];
    crypto_generichash(hash, sizeof(hash),
        (const unsigned char*)code.data(), code.size(),
        nullptr, 0);
    char hex[crypto_generichash_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex), hash, sizeof(hash));
    return std::string(hex);
}

// ============================================================
// HttpApi — constructor / run / stop
// ============================================================

HttpApi::HttpApi(const Config& cfg) : cfg_(cfg) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium init failed");
    }
    db_ = std::make_unique<PgDb>(cfg_.db_url);
    app_ = std::make_unique<crow::SimpleApp>();
}

HttpApi::~HttpApi() = default;

void HttpApi::run() {
    register_routes();
    std::cout << "[HTTP] Starting REST API on port " << cfg_.port << std::endl;
    app_->port(cfg_.port).multithreaded().run();
}

void HttpApi::stop() {
    app_->stop();
}

// ============================================================
// Helpers
// ============================================================

std::string HttpApi::extract_token(const std::string& auth_header) const {
    if (auth_header.substr(0, 7) == "Bearer ")
        return auth_header.substr(7);
    return "";
}

std::string HttpApi::authenticate_request(const std::string& auth_header) const {
    auto token = extract_token(auth_header);
    if (token.empty()) return "";
    return JwtHelper::verify(token, cfg_.jwt_secret);
}

std::string HttpApi::hash_password(const std::string& password) {
    char hash[crypto_pwhash_str_BYTES];
    if (crypto_pwhash_str(hash, password.c_str(), password.size(),
        crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("Password hashing failed (OOM?)");
    }
    return std::string(hash);
}

bool HttpApi::verify_password(const std::string& password,
                               const std::string& hash) {
    return crypto_pwhash_str_verify(hash.c_str(),
        password.c_str(), password.size()) == 0;
}

bool HttpApi::send_otp_email(const std::string& /*email*/,
                              const std::string& code) {
    // In production: integrate SMTP / SendGrid / Mailgun here.
    // For now we just log the OTP so you can test locally.
    std::cout << "[OTP] Code for email: " << code << std::endl;
    return true;
}

// ============================================================
// Route registration
// ============================================================

void HttpApi::register_routes() {
    route_health();
    route_send_otp();
    route_verify_otp();
    route_register();
    route_get_me();
    route_update_me();
    route_search_users();
    route_get_user();
    route_get_chats();
    route_open_direct();
    route_create_group();
    route_get_chat_info();
    route_get_messages();
    route_get_members();
    route_add_member();
    route_remove_member();
    route_search_messages();
    route_pin_message();
    route_react_message();
    route_mute_chat();
    route_chat_search_users();
    route_get_contacts();
    route_add_contact();
    route_remove_contact();
    route_check_contact();
    route_find_by_phone();
    route_explore_channels();
    route_my_channels();
    route_create_channel();
    route_get_channel();
    route_subscribe_channel();
    route_get_channel_posts();
    route_create_post();
    route_delete_post();
}

// ============================================================
// Health
// ============================================================

void HttpApi::route_health() {
    CROW_ROUTE((*app_), "/health")
    ([this]() {
        json body = {
            {"status", db_->connected() ? "ok" : "db_error"},
            {"db", db_->connected()},
            {"ts", std::time(nullptr)}
        };
        return json_resp(200, body);
    });
}

// ============================================================
// POST /api/auth/send-otp
// Body: { "email": "..." }
// Generates OTP, stores hash in DB otp_codes table (or users),
// and "sends" via email (console log for now).
// ============================================================

void HttpApi::route_send_otp() {
    CROW_ROUTE((*app_), "/api/auth/send-otp").methods("POST"_method)
    ([this](const crow::request& req) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        std::string email = body.value("email", "");
        if (email.empty()) return JSON_ERR(400, "email required");

        // Validate email format
        static const std::regex email_re(R"([^@\s]+@[^@\s]+\.[^@\s]+)");
        if (!std::regex_match(email, email_re))
            return JSON_ERR(400, "Invalid email format");

        std::string code = OtpHelper::generate();
        std::string code_hash = OtpHelper::hash(code);
        long long expires = std::time(nullptr) + 600; // 10 min

        try {
            // Upsert otp into a simple otp_codes table.
            // We create the table if it doesn't exist at startup.
            db_->exec(R"(
                INSERT INTO otp_codes (email, code_hash, expires_at)
                VALUES ($1, $2, to_timestamp($3))
                ON CONFLICT (email) DO UPDATE
                  SET code_hash = EXCLUDED.code_hash,
                      expires_at = EXCLUDED.expires_at,
                      attempts = 0
            )", {email, code_hash, std::to_string(expires)});
        } catch (const std::exception& e) {
            std::cerr << "[send-otp] DB error: " << e.what() << std::endl;
            return JSON_ERR(500, "Server error");
        }

        send_otp_email(email, code);

        return json_resp(200, {{"ok", true}, {"message", "OTP sent"}});
    });
}

// ============================================================
// POST /api/auth/verify-otp
// Body: { "email": "...", "code": "123456" }
// Returns { "token": "...", "needsRegistration": true/false }
// ============================================================

void HttpApi::route_verify_otp() {
    CROW_ROUTE((*app_), "/api/auth/verify-otp").methods("POST"_method)
    ([this](const crow::request& req) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        std::string email = body.value("email", "");
        std::string code  = body.value("code",  "");
        if (email.empty() || code.empty())
            return JSON_ERR(400, "email and code required");

        // Fetch OTP record
        std::vector<json> rows;
        try {
            rows = db_->query(R"(
                SELECT code_hash, expires_at, attempts
                FROM otp_codes
                WHERE email = $1
            )", {email});
        } catch (const std::exception& e) {
            return JSON_ERR(500, "Server error");
        }

        if (rows.empty())
            return JSON_ERR(400, "No OTP found for this email. Request a new one.");

        auto& row = rows[0];
        int attempts = std::stoi(row["attempts"].get<std::string>());
        if (attempts >= 5)
            return JSON_ERR(429, "Too many attempts. Request a new OTP.");

        // Increment attempts
        try {
            db_->exec("UPDATE otp_codes SET attempts = attempts + 1 WHERE email = $1",
                      {email});
        } catch (...) {}

        // Check expiry (PostgreSQL returns ISO8601 string)
        // We compare via a DB query to keep it simple
        std::vector<json> expiry_check;
        try {
            expiry_check = db_->query(R"(
                SELECT (expires_at > NOW()) AS valid FROM otp_codes WHERE email = $1
            )", {email});
        } catch (...) { return JSON_ERR(500, "Server error"); }
        if (expiry_check.empty() ||
            expiry_check[0]["valid"].get<std::string>() != "t")
            return JSON_ERR(400, "OTP expired. Request a new one.");

        // Verify hash
        std::string expected_hash = row["code_hash"].get<std::string>();
        std::string got_hash = OtpHelper::hash(code);
        if (expected_hash != got_hash)
            return JSON_ERR(400, "Invalid OTP code");

        // Delete OTP
        try {
            db_->exec("DELETE FROM otp_codes WHERE email = $1", {email});
        } catch (...) {}

        // Check if user exists
        std::vector<json> user_rows;
        try {
            user_rows = db_->query(
                "SELECT id, username, display_name FROM users WHERE email = $1",
                {email});
        } catch (const std::exception& e) {
            return JSON_ERR(500, "Server error");
        }

        if (user_rows.empty()) {
            // New user — return flag so Flutter shows register screen
            return json_resp(200, {{"needsRegistration", true}, {"email", email}});
        }

        // Existing user — issue JWT
        std::string user_id = user_rows[0]["id"].get<std::string>();
        std::string token = JwtHelper::create(user_id, cfg_.jwt_secret);
        return json_resp(200, {
            {"token", token},
            {"needsRegistration", false},
            {"user", {
                {"id", user_id},
                {"username", user_rows[0]["username"]},
                {"displayName", user_rows[0]["display_name"]}
            }}
        });
    });
}

// ============================================================
// POST /api/auth/register
// Body: { "email","username","displayName","phone"(opt) }
// Returns { "token": "..." }
// ============================================================

void HttpApi::route_register() {
    CROW_ROUTE((*app_), "/api/auth/register").methods("POST"_method)
    ([this](const crow::request& req) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        std::string email       = body.value("email", "");
        std::string username    = body.value("username", "");
        std::string display_name= body.value("displayName", username);
        std::string phone       = body.value("phone", "");

        if (email.empty() || username.empty())
            return JSON_ERR(400, "email and username required");

        // Validate username
        static const std::regex user_re(R"(^[a-z0-9_]{3,32}$)");
        if (!std::regex_match(username, user_re))
            return JSON_ERR(400, "Username must be 3-32 chars: a-z, 0-9, _");

        // Check duplicates
        try {
            auto dup_user = db_->query(
                "SELECT id FROM users WHERE username = $1", {username});
            if (!dup_user.empty())
                return JSON_ERR(409, "Username already taken");

            auto dup_email = db_->query(
                "SELECT id FROM users WHERE email = $1", {email});
            if (!dup_email.empty())
                return JSON_ERR(409, "Email already registered");

            if (!phone.empty()) {
                auto dup_phone = db_->query(
                    "SELECT id FROM users WHERE phone_number = $1", {phone});
                if (!dup_phone.empty())
                    return JSON_ERR(409, "Phone number already registered");
            }
        } catch (const std::exception& e) {
            std::cerr << "[register] DB check error: " << e.what() << std::endl;
            return JSON_ERR(500, "Server error");
        }

        // Generate dummy keys (real app: keys generated client-side)
        unsigned char pk[32], sk[32];
        crypto_box_keypair(pk, sk);
        char pk_hex[65];
        sodium_bin2hex(pk_hex, sizeof(pk_hex), pk, sizeof(pk));

        // Insert user — password fields are left empty (OTP auth, no password)
        std::string user_id;
        try {
            auto rows = db_->query(R"(
                INSERT INTO users
                  (username, email, phone_number, display_name,
                   password_hash, password_salt, public_key, is_verified)
                VALUES
                  ($1, $2, $3, $4, '', '', $5, true)
                RETURNING id
            )", {
                username,
                email,
                phone.empty() ? "" : phone,
                display_name,
                std::string(pk_hex)
            });
            if (rows.empty()) return JSON_ERR(500, "Insert failed");
            user_id = rows[0]["id"].get<std::string>();
        } catch (const std::exception& e) {
            std::cerr << "[register] Insert error: " << e.what() << std::endl;
            return JSON_ERR(500, "Server error: " + std::string(e.what()));
        }

        std::string token = JwtHelper::create(user_id, cfg_.jwt_secret);
        return json_resp(201, {{"token", token}, {"userId", user_id}});
    });
}

// ============================================================
// GET /api/auth/me
// ============================================================

void HttpApi::route_get_me() {
    CROW_ROUTE((*app_), "/api/auth/me")
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        try {
            auto rows = db_->query(R"(
                SELECT id, username, email, phone_number, display_name,
                       avatar_url, bio, status, status_text, created_at
                FROM users WHERE id = $1 AND is_deleted = false
            )", {uid});
            if (rows.empty()) return JSON_ERR(404, "User not found");
            auto& u = rows[0];
            json resp = {
                {"id",          u["id"]},
                {"username",    u["username"]},
                {"email",       u["email"]},
                {"phone",       u["phone_number"]},
                {"displayName", u["display_name"]},
                {"avatarUrl",   u["avatar_url"]},
                {"bio",         u["bio"]},
                {"status",      u["status"]},
                {"statusText",  u.count("status_text") ? u["status_text"] : nullptr},
                {"createdAt",   u["created_at"]}
            };
            return json_resp(200, resp);
        } catch (const std::exception& e) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// PATCH /api/auth/me
// ============================================================

void HttpApi::route_update_me() {
    CROW_ROUTE((*app_), "/api/auth/me").methods("PATCH"_method)
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        // Build dynamic UPDATE
        std::vector<std::string> parts, params;
        int idx = 1;
        auto add_field = [&](const std::string& col, const std::string& key) {
            if (body.contains(key) && !body[key].is_null()) {
                parts.push_back(col + " = $" + std::to_string(idx++));
                params.push_back(body[key].get<std::string>());
            }
        };
        add_field("display_name", "displayName");
        add_field("bio",          "bio");
        add_field("avatar_url",   "avatarUrl");
        add_field("status_text",  "statusText");
        add_field("phone_number", "phone");

        if (parts.empty()) return JSON_ERR(400, "Nothing to update");

        params.push_back(uid);
        std::string sql = "UPDATE users SET ";
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) sql += ", ";
            sql += parts[i];
        }
        sql += " WHERE id = $" + std::to_string(idx);

        try {
            db_->exec(sql, params);
            return json_resp(200, {{"ok", true}});
        } catch (const std::exception& e) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// GET /api/auth/users/search?q=...
// ============================================================

void HttpApi::route_search_users() {
    CROW_ROUTE((*app_), "/api/auth/users/search")
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        std::string q = req.url_params.get("q") ? req.url_params.get("q") : "";
        if (q.empty()) return json_resp(200, json::array());

        std::string like = "%" + q + "%";
        try {
            auto rows = db_->query(R"(
                SELECT id, username, display_name, avatar_url, status
                FROM users
                WHERE (username ILIKE $1 OR display_name ILIKE $1)
                  AND is_deleted = false AND id != $2
                LIMIT 20
            )", {like, uid});

            json arr = json::array();
            for (auto& r : rows) {
                arr.push_back({
                    {"id",          r["id"]},
                    {"username",    r["username"]},
                    {"displayName", r["display_name"]},
                    {"avatarUrl",   r["avatar_url"]},
                    {"status",      r["status"]}
                });
            }
            return json_resp(200, arr);
        } catch (...) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// GET /api/auth/users/:id
// ============================================================

void HttpApi::route_get_user() {
    CROW_ROUTE((*app_), "/api/auth/users/<string>")
    ([this](const crow::request& req, const std::string& user_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        try {
            auto rows = db_->query(R"(
                SELECT id, username, display_name, avatar_url, bio, status
                FROM users WHERE id = $1 AND is_deleted = false
            )", {user_id});
            if (rows.empty()) return JSON_ERR(404, "User not found");
            auto& u = rows[0];
            return json_resp(200, {
                {"id",          u["id"]},
                {"username",    u["username"]},
                {"displayName", u["display_name"]},
                {"avatarUrl",   u["avatar_url"]},
                {"bio",         u["bio"]},
                {"status",      u["status"]}
            });
        } catch (...) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// GET /api/chats
// ============================================================

void HttpApi::route_get_chats() {
    CROW_ROUTE((*app_), "/api/chats")
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        try {
            auto rows = db_->query(R"(
                SELECT c.id, c.chat_type, c.name, c.avatar_url,
                       c.updated_at,
                       (SELECT content FROM messages m
                        WHERE m.chat_id = c.id AND m.is_deleted = false
                        ORDER BY m.created_at DESC LIMIT 1) AS last_message,
                       (SELECT COUNT(*) FROM message_delivery md
                        JOIN messages msg ON md.message_id = msg.id
                        WHERE msg.chat_id = c.id AND md.recipient_id = $1
                          AND md.status != 'read') AS unread_count
                FROM chats c
                JOIN chat_members cm ON cm.chat_id = c.id
                WHERE cm.user_id = $1 AND cm.left_at IS NULL
                ORDER BY c.updated_at DESC
                LIMIT 100
            )", {uid});

            json arr = json::array();
            for (auto& r : rows) {
                arr.push_back({
                    {"id",           r["id"]},
                    {"type",         r["chat_type"]},
                    {"name",         r["name"]},
                    {"avatarUrl",    r["avatar_url"]},
                    {"lastMessage",  r["last_message"]},
                    {"unreadCount",  r["unread_count"]},
                    {"updatedAt",    r["updated_at"]}
                });
            }
            return json_resp(200, arr);
        } catch (const std::exception& e) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// POST /api/chats/direct
// Body: { "targetUserId": "..." }
// ============================================================

void HttpApi::route_open_direct() {
    CROW_ROUTE((*app_), "/api/chats/direct").methods("POST"_method)
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }
        std::string target_id = body.value("targetUserId", "");
        if (target_id.empty()) return JSON_ERR(400, "targetUserId required");

        try {
            // Check existing
            auto existing = db_->query(R"(
                SELECT c.id FROM chats c
                JOIN chat_members cm1 ON cm1.chat_id = c.id AND cm1.user_id = $1
                JOIN chat_members cm2 ON cm2.chat_id = c.id AND cm2.user_id = $2
                WHERE c.chat_type = 'private'
                LIMIT 1
            )", {uid, target_id});

            if (!existing.empty()) {
                return json_resp(200, {{"id", existing[0]["id"]}});
            }

            // Create new direct chat
            auto chat = db_->query(R"(
                INSERT INTO chats (chat_type, created_by)
                VALUES ('private', $1) RETURNING id
            )", {uid});
            std::string chat_id = chat[0]["id"].get<std::string>();

            db_->exec(R"(
                INSERT INTO chat_members (chat_id, user_id, role)
                VALUES ($1, $2, 'member'), ($1, $3, 'member')
            )", {chat_id, uid, target_id});

            return json_resp(201, {{"id", chat_id}});
        } catch (const std::exception& e) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// POST /api/chats/group
// Body: { "name", "memberIds": [], "description"? }
// ============================================================

void HttpApi::route_create_group() {
    CROW_ROUTE((*app_), "/api/chats/group").methods("POST"_method)
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        std::string name = body.value("name", "");
        std::string desc = body.value("description", "");
        if (name.empty()) return JSON_ERR(400, "name required");

        try {
            auto chat = db_->query(R"(
                INSERT INTO chats (chat_type, name, description, created_by)
                VALUES ('group', $1, $2, $3) RETURNING id
            )", {name, desc, uid});
            std::string chat_id = chat[0]["id"].get<std::string>();

            // Add creator as owner
            db_->exec(R"(
                INSERT INTO chat_members (chat_id, user_id, role, can_admin)
                VALUES ($1, $2, 'owner', true)
            )", {chat_id, uid});

            // Add members
            if (body.contains("memberIds") && body["memberIds"].is_array()) {
                for (auto& mid : body["memberIds"]) {
                    std::string member_id = mid.get<std::string>();
                    if (member_id == uid) continue;
                    try {
                        db_->exec(R"(
                            INSERT INTO chat_members (chat_id, user_id, role)
                            VALUES ($1, $2, 'member')
                            ON CONFLICT DO NOTHING
                        )", {chat_id, member_id});
                    } catch (...) {}
                }
            }

            return json_resp(201, {{"id", chat_id}, {"name", name}});
        } catch (const std::exception& e) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// GET /api/chats/:id
// ============================================================

void HttpApi::route_get_chat_info() {
    CROW_ROUTE((*app_), "/api/chats/<string>")
    ([this](const crow::request& req, const std::string& chat_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        try {
            // Verify membership
            auto mem = db_->query(
                "SELECT 1 FROM chat_members WHERE chat_id=$1 AND user_id=$2 AND left_at IS NULL",
                {chat_id, uid});
            if (mem.empty()) return JSON_ERR(403, "Not a member");

            auto rows = db_->query(R"(
                SELECT id, chat_type, name, description, avatar_url, created_at
                FROM chats WHERE id = $1
            )", {chat_id});
            if (rows.empty()) return JSON_ERR(404, "Chat not found");
            auto& c = rows[0];
            return json_resp(200, {
                {"id",          c["id"]},
                {"type",        c["chat_type"]},
                {"name",        c["name"]},
                {"description", c["description"]},
                {"avatarUrl",   c["avatar_url"]},
                {"createdAt",   c["created_at"]}
            });
        } catch (...) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// GET /api/chats/:id/messages?before=...
// ============================================================

void HttpApi::route_get_messages() {
    CROW_ROUTE((*app_), "/api/chats/<string>/messages")
    ([this](const crow::request& req, const std::string& chat_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        std::string before = req.url_params.get("before")
                             ? req.url_params.get("before") : "";

        try {
            auto mem = db_->query(
                "SELECT 1 FROM chat_members WHERE chat_id=$1 AND user_id=$2 AND left_at IS NULL",
                {chat_id, uid});
            if (mem.empty()) return JSON_ERR(403, "Not a member");

            std::vector<json> rows;
            if (before.empty()) {
                rows = db_->query(R"(
                    SELECT m.id, m.sender_id, m.content, m.content_type,
                           m.is_edited, m.is_pinned, m.reply_to_id, m.created_at,
                           u.username AS sender_username, u.display_name AS sender_name,
                           u.avatar_url AS sender_avatar
                    FROM messages m
                    JOIN users u ON u.id = m.sender_id
                    WHERE m.chat_id = $1 AND m.is_deleted = false
                    ORDER BY m.created_at DESC LIMIT 50
                )", {chat_id});
            } else {
                rows = db_->query(R"(
                    SELECT m.id, m.sender_id, m.content, m.content_type,
                           m.is_edited, m.is_pinned, m.reply_to_id, m.created_at,
                           u.username AS sender_username, u.display_name AS sender_name,
                           u.avatar_url AS sender_avatar
                    FROM messages m
                    JOIN users u ON u.id = m.sender_id
                    WHERE m.chat_id = $1 AND m.is_deleted = false
                      AND m.created_at < $2::timestamptz
                    ORDER BY m.created_at DESC LIMIT 50
                )", {chat_id, before});
            }

            json arr = json::array();
            for (auto& r : rows) {
                arr.push_back({
                    {"id",            r["id"]},
                    {"senderId",      r["sender_id"]},
                    {"senderName",    r["sender_name"]},
                    {"senderAvatar",  r["sender_avatar"]},
                    {"content",       r["content"]},
                    {"type",          r["content_type"]},
                    {"isEdited",      r["is_edited"]},
                    {"isPinned",      r["is_pinned"]},
                    {"replyToId",     r["reply_to_id"]},
                    {"createdAt",     r["created_at"]}
                });
            }
            return json_resp(200, arr);
        } catch (...) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// GET /api/chats/:id/members
// ============================================================

void HttpApi::route_get_members() {
    CROW_ROUTE((*app_), "/api/chats/<string>/members")
    ([this](const crow::request& req, const std::string& chat_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        try {
            auto rows = db_->query(R"(
                SELECT u.id, u.username, u.display_name, u.avatar_url, u.status,
                       cm.role, cm.joined_at
                FROM chat_members cm
                JOIN users u ON u.id = cm.user_id
                WHERE cm.chat_id = $1 AND cm.left_at IS NULL
                ORDER BY cm.joined_at ASC
            )", {chat_id});

            json arr = json::array();
            for (auto& r : rows) {
                arr.push_back({
                    {"id",          r["id"]},
                    {"username",    r["username"]},
                    {"displayName", r["display_name"]},
                    {"avatarUrl",   r["avatar_url"]},
                    {"status",      r["status"]},
                    {"role",        r["role"]},
                    {"joinedAt",    r["joined_at"]}
                });
            }
            return json_resp(200, arr);
        } catch (...) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// POST /api/chats/:id/members
// Body: { "userId": "..." }
// ============================================================

void HttpApi::route_add_member() {
    CROW_ROUTE((*app_), "/api/chats/<string>/members").methods("POST"_method)
    ([this](const crow::request& req, const std::string& chat_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        std::string target = body.value("userId", "");
        if (target.empty()) return JSON_ERR(400, "userId required");

        try {
            db_->exec(R"(
                INSERT INTO chat_members (chat_id, user_id, role)
                VALUES ($1, $2, 'member')
                ON CONFLICT (chat_id, user_id) DO UPDATE SET left_at = NULL
            )", {chat_id, target});
            return json_resp(200, {{"ok", true}});
        } catch (...) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// DELETE /api/chats/:id/members/:uid
// ============================================================

void HttpApi::route_remove_member() {
    CROW_ROUTE((*app_), "/api/chats/<string>/members/<string>")
        .methods("DELETE"_method)
    ([this](const crow::request& req, const std::string& chat_id,
            const std::string& target_uid) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        try {
            db_->exec(R"(
                UPDATE chat_members SET left_at = NOW()
                WHERE chat_id = $1 AND user_id = $2
            )", {chat_id, target_uid});
            return json_resp(200, {{"ok", true}});
        } catch (...) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// GET /api/chats/:id/search?q=...
// ============================================================

void HttpApi::route_search_messages() {
    CROW_ROUTE((*app_), "/api/chats/<string>/search")
    ([this](const crow::request& req, const std::string& chat_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        std::string q = req.url_params.get("q") ? req.url_params.get("q") : "";
        if (q.empty()) return json_resp(200, json::array());

        try {
            auto rows = db_->query(R"(
                SELECT id, sender_id, content, created_at
                FROM messages
                WHERE chat_id = $1 AND content ILIKE $2 AND is_deleted = false
                ORDER BY created_at DESC LIMIT 50
            )", {chat_id, "%" + q + "%"});

            json arr = json::array();
            for (auto& r : rows) arr.push_back(r);
            return json_resp(200, arr);
        } catch (...) {
            return JSON_ERR(500, "Server error");
        }
    });
}

// ============================================================
// POST /api/chats/:id/messages/:mid/pin
// ============================================================

void HttpApi::route_pin_message() {
    CROW_ROUTE((*app_), "/api/chats/<string>/messages/<string>/pin")
        .methods("POST"_method)
    ([this](const crow::request& req, const std::string& /*chat_id*/,
            const std::string& msg_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            db_->exec("UPDATE messages SET is_pinned = NOT is_pinned WHERE id = $1",
                      {msg_id});
            return json_resp(200, {{"ok", true}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

// ============================================================
// POST /api/chats/:id/messages/:mid/react
// Body: { "emoji": "👍" }
// ============================================================

void HttpApi::route_react_message() {
    CROW_ROUTE((*app_), "/api/chats/<string>/messages/<string>/react")
        .methods("POST"_method)
    ([this](const crow::request& req, const std::string& /*chat_id*/,
            const std::string& msg_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }
        std::string emoji = body.value("emoji", "");
        if (emoji.empty()) return JSON_ERR(400, "emoji required");

        try {
            db_->exec(R"(
                INSERT INTO reactions (message_id, user_id, emoji)
                VALUES ($1, $2, $3)
                ON CONFLICT (message_id, user_id, emoji) DO DELETE
            )", {msg_id, uid, emoji});
            return json_resp(200, {{"ok", true}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

// ============================================================
// POST /api/chats/:id/mute
// ============================================================

void HttpApi::route_mute_chat() {
    CROW_ROUTE((*app_), "/api/chats/<string>/mute").methods("POST"_method)
    ([this](const crow::request& req, const std::string& chat_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            db_->exec(R"(
                UPDATE chat_members
                SET muted_until = CASE
                    WHEN muted_until IS NULL OR muted_until < NOW()
                    THEN NOW() + INTERVAL '1 year'
                    ELSE NULL
                END
                WHERE chat_id = $1 AND user_id = $2
            )", {chat_id, uid});
            return json_resp(200, {{"ok", true}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

// ============================================================
// GET /api/chats/users/search?q=...
// ============================================================

void HttpApi::route_chat_search_users() {
    CROW_ROUTE((*app_), "/api/chats/users/search")
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        std::string q = req.url_params.get("q") ? req.url_params.get("q") : "";
        std::string like = "%" + q + "%";
        try {
            auto rows = db_->query(R"(
                SELECT id, username, display_name, avatar_url
                FROM users
                WHERE (username ILIKE $1 OR display_name ILIKE $1)
                  AND is_deleted = false AND id != $2
                LIMIT 20
            )", {like, uid});

            json arr = json::array();
            for (auto& r : rows) arr.push_back({
                {"id", r["id"]}, {"username", r["username"]},
                {"displayName", r["display_name"]}, {"avatarUrl", r["avatar_url"]}
            });
            return json_resp(200, arr);
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

// ============================================================
// Contacts
// ============================================================

void HttpApi::route_get_contacts() {
    CROW_ROUTE((*app_), "/api/contacts")
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            auto rows = db_->query(R"(
                SELECT u.id, u.username, u.display_name, u.avatar_url, u.status,
                       c.nickname, c.is_favorite
                FROM contacts c
                JOIN users u ON u.id = c.contact_id
                WHERE c.user_id = $1 AND c.is_blocked = false
                ORDER BY u.username ASC
            )", {uid});

            json arr = json::array();
            for (auto& r : rows) arr.push_back({
                {"id",          r["id"]},
                {"username",    r["username"]},
                {"displayName", r["display_name"]},
                {"avatarUrl",   r["avatar_url"]},
                {"status",      r["status"]},
                {"nickname",    r["nickname"]},
                {"isFavorite",  r["is_favorite"]}
            });
            return json_resp(200, arr);
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_add_contact() {
    CROW_ROUTE((*app_), "/api/contacts").methods("POST"_method)
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        std::string contact_id = body.value("contactId", "");
        std::string nickname   = body.value("nickname", "");
        if (contact_id.empty()) return JSON_ERR(400, "contactId required");
        if (contact_id == uid)  return JSON_ERR(400, "Cannot add yourself");

        try {
            db_->exec(R"(
                INSERT INTO contacts (user_id, contact_id, nickname)
                VALUES ($1, $2, $3)
                ON CONFLICT (user_id, contact_id) DO UPDATE SET nickname = EXCLUDED.nickname
            )", {uid, contact_id, nickname});
            return json_resp(201, {{"ok", true}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_remove_contact() {
    CROW_ROUTE((*app_), "/api/contacts/<string>").methods("DELETE"_method)
    ([this](const crow::request& req, const std::string& contact_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            db_->exec("DELETE FROM contacts WHERE user_id=$1 AND contact_id=$2",
                      {uid, contact_id});
            return json_resp(200, {{"ok", true}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_check_contact() {
    CROW_ROUTE((*app_), "/api/contacts/check/<string>")
    ([this](const crow::request& req, const std::string& contact_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            auto rows = db_->query(
                "SELECT 1 FROM contacts WHERE user_id=$1 AND contact_id=$2",
                {uid, contact_id});
            return json_resp(200, {{"isContact", !rows.empty()}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_find_by_phone() {
    CROW_ROUTE((*app_), "/api/contacts/find-by-phone")
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        std::string phone = req.url_params.get("phone")
                            ? req.url_params.get("phone") : "";
        if (phone.empty()) return JSON_ERR(400, "phone required");

        try {
            auto rows = db_->query(R"(
                SELECT id, username, display_name, avatar_url
                FROM users WHERE phone_number = $1 AND is_deleted = false
            )", {phone});
            if (rows.empty()) return JSON_ERR(404, "User not found");
            auto& u = rows[0];
            return json_resp(200, {
                {"id",          u["id"]},
                {"username",    u["username"]},
                {"displayName", u["display_name"]},
                {"avatarUrl",   u["avatar_url"]}
            });
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

// ============================================================
// Channels
// ============================================================

void HttpApi::route_explore_channels() {
    CROW_ROUTE((*app_), "/api/channels/explore")
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        std::string q = req.url_params.get("q") ? req.url_params.get("q") : "";
        std::string like = "%" + q + "%";
        try {
            auto rows = db_->query(R"(
                SELECT c.id, c.username, c.name, c.description, c.avatar_url,
                       c.is_public, c.subscriber_count
                FROM channels c
                WHERE c.is_public = true
                  AND ($1 = '%%' OR c.name ILIKE $1 OR c.username ILIKE $1)
                ORDER BY c.subscriber_count DESC LIMIT 50
            )", {like});

            json arr = json::array();
            for (auto& r : rows) arr.push_back(r);
            return json_resp(200, arr);
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_my_channels() {
    CROW_ROUTE((*app_), "/api/channels/my")
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            auto rows = db_->query(R"(
                SELECT c.id, c.username, c.name, c.avatar_url, cs.role
                FROM channel_subscribers cs
                JOIN channels c ON c.id = cs.channel_id
                WHERE cs.user_id = $1
                ORDER BY c.name ASC
            )", {uid});
            json arr = json::array();
            for (auto& r : rows) arr.push_back(r);
            return json_resp(200, arr);
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_create_channel() {
    CROW_ROUTE((*app_), "/api/channels").methods("POST"_method)
    ([this](const crow::request& req) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        std::string username    = body.value("username", "");
        std::string name        = body.value("name", "");
        std::string description = body.value("description", "");
        bool is_public          = body.value("isPublic", true);

        if (username.empty() || name.empty())
            return JSON_ERR(400, "username and name required");

        try {
            auto rows = db_->query(R"(
                INSERT INTO channels (username, name, description, is_public, owner_id)
                VALUES ($1, $2, $3, $4, $5) RETURNING id
            )", {username, name, description, is_public ? "true" : "false", uid});

            std::string chan_id = rows[0]["id"].get<std::string>();
            db_->exec(R"(
                INSERT INTO channel_subscribers (channel_id, user_id, role)
                VALUES ($1, $2, 'owner')
            )", {chan_id, uid});

            return json_resp(201, {{"id", chan_id}, {"username", username}});
        } catch (const std::exception& e) {
            std::string err = e.what();
            if (err.find("unique") != std::string::npos)
                return JSON_ERR(409, "Username already taken");
            return JSON_ERR(500, "Server error");
        }
    });
}

void HttpApi::route_get_channel() {
    CROW_ROUTE((*app_), "/api/channels/<string>")
    ([this](const crow::request& req, const std::string& username) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            auto rows = db_->query(R"(
                SELECT id, username, name, description, avatar_url,
                       is_public, subscriber_count, owner_id
                FROM channels WHERE username = $1
            )", {username});
            if (rows.empty()) return JSON_ERR(404, "Channel not found");
            return json_resp(200, rows[0]);
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_subscribe_channel() {
    CROW_ROUTE((*app_), "/api/channels/<string>/subscribe").methods("POST"_method)
    ([this](const crow::request& req, const std::string& channel_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            db_->exec(R"(
                INSERT INTO channel_subscribers (channel_id, user_id, role)
                VALUES ($1, $2, 'subscriber')
                ON CONFLICT DO NOTHING
            )", {channel_id, uid});
            db_->exec(R"(
                UPDATE channels SET subscriber_count = subscriber_count + 1
                WHERE id = $1
            )", {channel_id});
            return json_resp(200, {{"ok", true}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_get_channel_posts() {
    CROW_ROUTE((*app_), "/api/channels/<string>/posts")
    ([this](const crow::request& req, const std::string& channel_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        std::string before = req.url_params.get("before")
                             ? req.url_params.get("before") : "";
        try {
            std::vector<json> rows;
            if (before.empty()) {
                rows = db_->query(R"(
                    SELECT id, channel_id, content, media_urls, is_paid, created_at
                    FROM channel_posts WHERE channel_id = $1
                    ORDER BY created_at DESC LIMIT 30
                )", {channel_id});
            } else {
                rows = db_->query(R"(
                    SELECT id, channel_id, content, media_urls, is_paid, created_at
                    FROM channel_posts
                    WHERE channel_id = $1 AND created_at < $2::timestamptz
                    ORDER BY created_at DESC LIMIT 30
                )", {channel_id, before});
            }
            json arr = json::array();
            for (auto& r : rows) arr.push_back(r);
            return json_resp(200, arr);
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_create_post() {
    CROW_ROUTE((*app_), "/api/channels/<string>/posts").methods("POST"_method)
    ([this](const crow::request& req, const std::string& channel_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return JSON_ERR(400, "Invalid JSON"); }

        std::string content = body.value("content", "");
        bool is_paid = body.value("isPaid", false);

        try {
            // Check owner/admin
            auto perm = db_->query(
                "SELECT role FROM channel_subscribers WHERE channel_id=$1 AND user_id=$2",
                {channel_id, uid});
            if (perm.empty() ||
                (perm[0]["role"].get<std::string>() != "owner" &&
                 perm[0]["role"].get<std::string>() != "admin"))
                return JSON_ERR(403, "Not a channel admin");

            db_->exec(R"(
                INSERT INTO channel_posts (channel_id, content, is_paid, author_id)
                VALUES ($1, $2, $3, $4)
            )", {channel_id, content, is_paid ? "true" : "false", uid});

            return json_resp(201, {{"ok", true}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}

void HttpApi::route_delete_post() {
    CROW_ROUTE((*app_), "/api/channels/<string>/posts/<string>")
        .methods("DELETE"_method)
    ([this](const crow::request& req, const std::string& channel_id,
            const std::string& post_id) {
        std::string uid = authenticate_request(req.get_header_value("Authorization"));
        if (uid.empty()) return JSON_ERR(401, "Unauthorized");
        try {
            db_->exec(
                "DELETE FROM channel_posts WHERE id=$1 AND channel_id=$2",
                {post_id, channel_id});
            return json_resp(200, {{"ok", true}});
        } catch (...) { return JSON_ERR(500, "Server error"); }
    });
}
