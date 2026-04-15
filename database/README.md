# 📊 Database Schema

PostgreSQL 15+ database schema for Premium Messenger with optimized structure, indices, and triggers.

**Status:** ✅ Production Ready | **Tables:** 14 | **Indices:** 20+ | **Lines:** 461

---

## 📋 Quick Start

### Apply Schema

```bash
# Local development
psql -h localhost -U messenger -d messenger_db < schema.sql

# Production
psql -h prod-db.example.com -U messenger -d messenger_db < schema.sql
```

### Create Database

```sql
CREATE DATABASE messenger_db;
-- Then apply schema.sql
```

### Verify Installation

```bash
psql -h localhost -U messenger -d messenger_db -c "\\dt"
```

---

## 🏗️ Tables

### 1. users (User Accounts)
```sql
- id (TEXT, PRIMARY KEY)
- username (VARCHAR(255), UNIQUE)
- email (VARCHAR(255), UNIQUE)
- password_hash (VARCHAR(255))
- created_at (TIMESTAMP DEFAULT NOW())
- updated_at (TIMESTAMP DEFAULT NOW())
- phone (VARCHAR(20))
- avatar_url (TEXT)
- status (VARCHAR(50): online, offline, away, dnd)
- public_key (BYTEA)  -- X25519 public key for E2EE
- is_verified (BOOLEAN)
- is_active (BOOLEAN)
- last_seen (TIMESTAMP)
```

### 2. sessions (Active Sessions)
```sql
- id (TEXT, PRIMARY KEY)
- user_id (TEXT, FOREIGN KEY)
- device_type (VARCHAR(50): web, mobile, desktop)
- device_name (VARCHAR(255))
- token (TEXT, UNIQUE)
- ip_address (INET)
- user_agent (TEXT)
- is_active (BOOLEAN)
- expires_at (TIMESTAMP)
- created_at (TIMESTAMP DEFAULT NOW())
- last_activity (TIMESTAMP)
```

### 3. chats (Conversations)
```sql
- id (TEXT, PRIMARY KEY)
- type (VARCHAR(10): 1-1, group, channel)
- name (VARCHAR(255))
- description (TEXT)
- avatar_url (TEXT)
- is_encrypted (BOOLEAN)
- created_by (TEXT, FOREIGN KEY)
- created_at (TIMESTAMP DEFAULT NOW())
- updated_at (TIMESTAMP DEFAULT NOW())
```

### 4. chat_members (Chat Participation)
```sql
- id (TEXT, PRIMARY KEY)
- chat_id (TEXT, FOREIGN KEY)
- user_id (TEXT, FOREIGN KEY)
- role (VARCHAR(20): owner, admin, member)
- can_send_messages (BOOLEAN)
- can_delete_messages (BOOLEAN)
- joined_at (TIMESTAMP DEFAULT NOW())
- mute_until (TIMESTAMP)  -- NULL if not muted
- INDEX: (chat_id, user_id)
```

### 5. messages (Chat Messages)
```sql
- id (TEXT, PRIMARY KEY)
- chat_id (TEXT, FOREIGN KEY)
- from_user_id (TEXT, FOREIGN KEY)
- content (TEXT)  -- encrypted in transit
- reply_to_id (TEXT, FOREIGN KEY, NULLABLE)
- forward_from_id (TEXT, FOREIGN KEY, NULLABLE)
- message_type (VARCHAR(20): text, image, video, file)
- is_pinned (BOOLEAN)
- self_destruct_at (TIMESTAMP, NULLABLE)
- created_at (TIMESTAMP DEFAULT NOW())
- updated_at (TIMESTAMP)
- deleted_at (TIMESTAMP, NULLABLE)  -- soft delete
- INDEX: (chat_id, created_at)
```

### 6. message_delivery (Per-Recipient Status)
```sql
- id (TEXT, PRIMARY KEY)
- message_id (TEXT, FOREIGN KEY)
- to_user_id (TEXT, FOREIGN KEY)
- status (VARCHAR(20): pending, sent, delivered, read)
- read_at (TIMESTAMP, NULLABLE)
- delivered_at (TIMESTAMP, NULLABLE)
- INDEX: (message_id, to_user_id)
```

### 7. reactions (Emoji Reactions)
```sql
- id (TEXT, PRIMARY KEY)
- message_id (TEXT, FOREIGN KEY)
- user_id (TEXT, FOREIGN KEY)
- emoji (VARCHAR(20))
- created_at (TIMESTAMP DEFAULT NOW())
- UNIQUE: (message_id, user_id, emoji)
```

### 8. contacts (Contact List)
```sql
- id (TEXT, PRIMARY KEY)
- user_id (TEXT, FOREIGN KEY)
- contact_user_id (TEXT, FOREIGN KEY)
- nickname (VARCHAR(255), NULLABLE)
- is_favorite (BOOLEAN)
- is_blocked (BOOLEAN)
- created_at (TIMESTAMP DEFAULT NOW())
- UNIQUE: (user_id, contact_user_id)
```

### 9. calls (Voice/Video History)
```sql
- id (TEXT, PRIMARY KEY)
- from_user_id (TEXT, FOREIGN KEY)
- to_user_id (TEXT, FOREIGN KEY)
- call_type (VARCHAR(20): voice, video)
- duration_seconds (INTEGER)
- status (VARCHAR(20): ringing, answered, missed, declined)
- started_at (TIMESTAMP)
- ended_at (TIMESTAMP, NULLABLE)
- INDEX: (from_user_id, created_at)
```

### 10. encryption_keys (Key Management)
```sql
- id (TEXT, PRIMARY KEY)
- user_id (TEXT, FOREIGN KEY)
- public_key (BYTEA)  -- X25519
- private_key_encrypted (BYTEA)  -- Encrypted with user password
- key_type (VARCHAR(50): identity, prekey, signedprekey)
- is_active (BOOLEAN)
- created_at (TIMESTAMP DEFAULT NOW())
- expires_at (TIMESTAMP, NULLABLE)
- backup_key (BYTEA, NULLABLE)
- INDEX: (user_id, is_active)
```

### 11. file_uploads (Media Storage)
```sql
- id (TEXT, PRIMARY KEY)
- message_id (TEXT, FOREIGN KEY)
- file_name (VARCHAR(255))
- file_size (BIGINT)
- mime_type (VARCHAR(100))
- upload_url (TEXT)
- width (INTEGER, NULLABLE)  -- For images/videos
- height (INTEGER, NULLABLE)
- duration_seconds (INTEGER, NULLABLE)  -- For videos
- uploaded_at (TIMESTAMP DEFAULT NOW())
```

### 12. audit_log (Security Audit)
```sql
- id (TEXT, PRIMARY KEY)
- user_id (TEXT, FOREIGN KEY, NULLABLE)
- action (VARCHAR(100))  -- login, logout, delete_chat, etc.
- resource_type (VARCHAR(50))
- resource_id (TEXT, NULLABLE)
- details (JSONB)  -- Additional context
- ip_address (INET, NULLABLE)
- created_at (TIMESTAMP DEFAULT NOW())
- INDEX: (user_id, created_at)
```

### 13. notifications (Push Notifications)
```sql
- id (TEXT, PRIMARY KEY)
- user_id (TEXT, FOREIGN KEY)
- type (VARCHAR(50))  -- message, reaction, call, etc.
- title (VARCHAR(255))
- body (TEXT)
- data (JSONB)
- is_read (BOOLEAN)
- created_at (TIMESTAMP DEFAULT NOW())
- INDEX: (user_id, is_read)
```

### 14. blocklist (Blocked Users)
```sql
- id (TEXT, PRIMARY KEY)
- user_id (TEXT, FOREIGN KEY)
- blocked_user_id (TEXT, FOREIGN KEY)
- reason (TEXT, NULLABLE)
- created_at (TIMESTAMP DEFAULT NOW())
- UNIQUE: (user_id, blocked_user_id)
```

---

## 🔑 Indices

**Performance Optimizations:**
```sql
-- Messages
CREATE INDEX idx_messages_chat_created 
  ON messages(chat_id, created_at DESC);
CREATE INDEX idx_messages_from_user 
  ON messages(from_user_id);

-- Sessions
CREATE INDEX idx_sessions_user_active 
  ON sessions(user_id, is_active);
CREATE INDEX idx_sessions_token 
  ON sessions(token);

-- Delivery
CREATE INDEX idx_message_delivery_status 
  ON message_delivery(status);
CREATE INDEX idx_message_delivery_user 
  ON message_delivery(to_user_id, created_at DESC);

-- Chat Members
CREATE INDEX idx_chat_members_user 
  ON chat_members(user_id);

-- Audit
CREATE INDEX idx_audit_log_user_action 
  ON audit_log(user_id, created_at DESC);

-- Plus 10+ more for optimal query performance
```

---

## 🔐 Security Features

### Password Storage
- bcrypt hashing (never plain text)
- Salt: auto-generated
- Work factor: 12 rounds

### Encryption
- X25519 ECDH for key exchange
- ChaCha20-Poly1305 for message encryption
- Private keys encrypted at rest

### Audit Logging
- All sensitive actions logged
- IP tracking for security
- JSONB for flexible logging

### Access Control
- Row-level security (RLS) ready
- User-isolated data
- Audit trail for compliance

---

## 🔄 Views

### active_sessions
Shows only active sessions with user info
```sql
SELECT sessions.*, users.username 
FROM sessions 
JOIN users ON sessions.user_id = users.id 
WHERE sessions.is_active = true;
```

### user_presence
Real-time user status and last activity
```sql
SELECT id, username, status, last_seen 
FROM users 
ORDER BY last_seen DESC;
```

### unread_messages
Count of unread messages per user/chat
```sql
SELECT to_user_id, COUNT(*) as unread_count 
FROM message_delivery 
WHERE status != 'read' 
GROUP BY to_user_id;
```

---

## ⚙️ Triggers

### auto_updated_at
Automatically updates `updated_at` timestamp on row changes
```sql
CREATE TRIGGER trigger_users_updated_at
BEFORE UPDATE ON users
FOR EACH ROW
EXECUTE FUNCTION update_updated_at_column();
-- Applied to: users, chats, contacts
```

---

## 📊 Query Examples

### Get chat messages with read status
```sql
SELECT m.*, md.status as delivery_status
FROM messages m
LEFT JOIN message_delivery md ON m.id = md.message_id
WHERE m.chat_id = $1
ORDER BY m.created_at DESC
LIMIT 50;
```

### Get active users online now
```sql
SELECT id, username, avatar_url, status
FROM users
WHERE status = 'online'
  AND last_seen > NOW() - INTERVAL '5 minutes'
ORDER BY last_seen DESC;
```

### Get user contacts with online status
```sql
SELECT u.id, u.username, u.avatar_url, u.status,
       c.nickname, c.is_favorite
FROM contacts c
JOIN users u ON c.contact_user_id = u.id
WHERE c.user_id = $1 AND c.is_blocked = false
ORDER BY c.is_favorite DESC, u.username;
```

### Get unread message count per chat
```sql
SELECT m.chat_id, COUNT(*) as unread_count
FROM messages m
LEFT JOIN message_delivery md ON m.id = md.message_id
WHERE md.to_user_id = $1 AND md.status != 'read'
GROUP BY m.chat_id;
```

---

## 🔧 Maintenance

### Backup
```bash
# Daily backup
pg_dump -U messenger messenger_db | gzip > backup_$(date +%Y%m%d).sql.gz

# Production backup
pg_dump -h prod-db.example.com -U messenger --format=custom > backup.dump
```

### Restore
```bash
# From backup
psql -U messenger messenger_db < backup.sql
```

### Cleanup
```sql
-- Delete old audit logs (older than 1 year)
DELETE FROM audit_log WHERE created_at < NOW() - INTERVAL '1 year';

-- Delete expired sessions
DELETE FROM sessions WHERE expires_at < NOW();

-- Delete soft-deleted messages older than 30 days
DELETE FROM messages WHERE deleted_at < NOW() - INTERVAL '30 days';
```

### Performance Analysis
```sql
-- Analyze query performance
EXPLAIN ANALYZE
SELECT m.*, md.status FROM messages m
LEFT JOIN message_delivery md ON m.id = md.message_id
WHERE m.chat_id = 'chat_123'
ORDER BY m.created_at DESC LIMIT 50;

-- Check index usage
SELECT schemaname, tablename, indexname, idx_scan
FROM pg_stat_user_indexes
ORDER BY idx_scan DESC;
```

---

## 📈 Scalability

### Current
- Up to 1 million users
- Up to 100 million messages
- Single PostgreSQL instance

### Scaling Strategies
1. **Read Replicas** - For SELECT queries
2. **Partitioning** - messages table by created_at
3. **Sharding** - By user_id for ultra-scale
4. **Time-series DB** - ScyllaDB for message archive

---

## 🚀 Migration

### From Old Schema

```sql
-- 1. Backup old database
pg_dump old_messenger > old_backup.sql

-- 2. Create new database
CREATE DATABASE messenger_db;

-- 3. Apply new schema
psql -d messenger_db < schema.sql

-- 4. Migrate data (if compatible)
INSERT INTO users SELECT * FROM old_db.users;
```

---

## 📞 Support

**Utilities:**
- pgAdmin: http://localhost:5050 (local dev)
- Adminer: http://localhost:8080 (alternative UI)
- psql CLI: `psql -h localhost -U messenger -d messenger_db`

**Documentation:**
- PostgreSQL: https://www.postgresql.org/docs/15/
- Full-text search: https://www.postgresql.org/docs/15/textsearch.html

---

**Version:** 2.0.0  
**Updated:** April 15, 2024  
**DB Version:** PostgreSQL 15+
