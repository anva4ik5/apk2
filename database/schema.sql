-- ============================================================================
-- Premium Messenger Database Schema
-- PostgreSQL 15+
-- ============================================================================

-- Enable extensions
CREATE EXTENSION IF NOT EXISTS pgcrypto;
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- ============================================================================
-- USERS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS users (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    username VARCHAR(255) UNIQUE NOT NULL,
    email VARCHAR(255) UNIQUE NOT NULL,
    phone_number VARCHAR(20) UNIQUE,
    
    -- Authentication
    password_hash VARCHAR(255) NOT NULL,
    password_salt VARCHAR(255) NOT NULL,
    
    -- Profile
    first_name VARCHAR(100),
    last_name VARCHAR(100),
    avatar_url TEXT,
    bio TEXT,
    status VARCHAR(50) DEFAULT 'offline', -- 'online', 'away', 'offline', 'dnd'
    
    -- E2E Encryption
    public_key TEXT NOT NULL,  -- X25519 public key (hex)
    public_key_signature TEXT,  -- Ed25519 signature of public key
    encryption_algorithm VARCHAR(50) DEFAULT 'chacha20poly1305',
    
    -- Security
    two_factor_enabled BOOLEAN DEFAULT false,
    two_factor_secret VARCHAR(255),
    
    -- Profile Settings
    is_verified BOOLEAN DEFAULT false,
    is_blocked BOOLEAN DEFAULT false,
    is_deleted BOOLEAN DEFAULT false,
    
    -- Metadata
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    last_seen TIMESTAMP WITH TIME ZONE,
    
    CONSTRAINT username_length CHECK (LENGTH(username) >= 3),
    CONSTRAINT email_format CHECK (email ~ '^[^@]+@[^@]+\.[^@]+$')
);

CREATE INDEX idx_users_username ON users(username);
CREATE INDEX idx_users_email ON users(email);
CREATE INDEX idx_users_status ON users(status);

-- ============================================================================
-- SESSIONS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS sessions (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- Session Info
    token VARCHAR(255) UNIQUE NOT NULL,
    device_name VARCHAR(100),
    device_type VARCHAR(50), -- 'mobile', 'desktop', 'web'
    device_os VARCHAR(50),
    ip_address INET,
    user_agent TEXT,
    
    -- Session State
    is_active BOOLEAN DEFAULT true,
    last_activity TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    -- Expiration
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP WITH TIME ZONE DEFAULT (CURRENT_TIMESTAMP + INTERVAL '30 days'),
    
    CONSTRAINT token_not_empty CHECK (LENGTH(token) > 0)
);

CREATE INDEX idx_sessions_user_id ON sessions(user_id);
CREATE INDEX idx_sessions_token ON sessions(token);
CREATE INDEX idx_sessions_active ON sessions(is_active);

-- ============================================================================
-- CHATS TABLE (1-to-1 & Groups)
-- ============================================================================

CREATE TABLE IF NOT EXISTS chats (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    chat_type VARCHAR(50) NOT NULL, -- 'private', 'group', 'channel', 'supergroup'
    
    -- Group/Channel Info
    name VARCHAR(255),
    description TEXT,
    avatar_url TEXT,
    
    -- Group Settings
    is_encrypted BOOLEAN DEFAULT true,
    max_members INT,
    
    -- Metadata
    created_by UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    CONSTRAINT name_required_for_groups CHECK (
        (chat_type = 'private') OR (name IS NOT NULL)
    )
);

CREATE INDEX idx_chats_type ON chats(chat_type);
CREATE INDEX idx_chats_created_by ON chats(created_by);

-- ============================================================================
-- CHAT MEMBERS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS chat_members (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    chat_id UUID NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- Member Info
    role VARCHAR(50) DEFAULT 'member', -- 'owner', 'admin', 'moderator', 'member'
    display_name VARCHAR(100),
    
    -- Permissions
    can_send_messages BOOLEAN DEFAULT true,
    can_edit_messages BOOLEAN DEFAULT false,
    can_delete_messages BOOLEAN DEFAULT false,
    can_pin_messages BOOLEAN DEFAULT false,
    can_admin BOOLEAN DEFAULT false,
    
    -- Metadata
    joined_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    left_at TIMESTAMP WITH TIME ZONE,
    muted_until TIMESTAMP WITH TIME ZONE,
    
    UNIQUE(chat_id, user_id)
);

CREATE INDEX idx_chat_members_chat_id ON chat_members(chat_id);
CREATE INDEX idx_chat_members_user_id ON chat_members(user_id);

-- ============================================================================
-- MESSAGES TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS messages (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    chat_id UUID NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    sender_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- Message Content
    content TEXT NOT NULL,  -- Usually encrypted
    content_type VARCHAR(50) DEFAULT 'text', -- 'text', 'photo', 'video', 'audio', 'document'
    
    -- Message State
    is_edited BOOLEAN DEFAULT false,
    is_deleted BOOLEAN DEFAULT false,
    is_pinned BOOLEAN DEFAULT false,
    is_self_destructing BOOLEAN DEFAULT false,
    self_destruct_seconds INT,
    
    -- Forward/Reply
    reply_to_id UUID REFERENCES messages(id) ON DELETE SET NULL,
    forward_from_id UUID,
    
    -- Metadata
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    edited_at TIMESTAMP WITH TIME ZONE,
    deleted_at TIMESTAMP WITH TIME ZONE,
    
    CONSTRAINT content_not_empty CHECK (LENGTH(content) > 0 OR content_type != 'text')
);

CREATE INDEX idx_messages_chat_id ON messages(chat_id);
CREATE INDEX idx_messages_sender_id ON messages(sender_id);
CREATE INDEX idx_messages_created_at ON messages(created_at DESC);
CREATE INDEX idx_messages_reply_to ON messages(reply_to_id);

-- ============================================================================
-- MESSAGE DELIVERY STATUS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS message_delivery (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    message_id UUID NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    recipient_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- Delivery Status
    status VARCHAR(50) DEFAULT 'pending', -- 'pending', 'sent', 'delivered', 'read'
    
    -- Timestamps
    sent_at TIMESTAMP WITH TIME ZONE,
    delivered_at TIMESTAMP WITH TIME ZONE,
    read_at TIMESTAMP WITH TIME ZONE,
    
    UNIQUE(message_id, recipient_id)
);

CREATE INDEX idx_msg_delivery_message_id ON message_delivery(message_id);
CREATE INDEX idx_msg_delivery_recipient_id ON message_delivery(recipient_id);
CREATE INDEX idx_msg_delivery_status ON message_delivery(status);

-- ============================================================================
-- REACTIONS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS reactions (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    message_id UUID NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- Reaction Data
    emoji VARCHAR(10) NOT NULL, -- '❤️', '👍', etc. or custom emoji ID
    reaction_type VARCHAR(50) DEFAULT 'emoji', -- 'emoji', 'custom'
    
    -- Metadata
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    UNIQUE(message_id, user_id, emoji)
);

CREATE INDEX idx_reactions_message_id ON reactions(message_id);
CREATE INDEX idx_reactions_user_id ON reactions(user_id);

-- ============================================================================
-- CONTACTS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS contacts (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    contact_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- Contact Info
    nickname VARCHAR(100),
    is_blocked BOOLEAN DEFAULT false,
    is_favorite BOOLEAN DEFAULT false,
    
    -- Metadata
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    
    UNIQUE(user_id, contact_id),
    CONSTRAINT cant_add_self CHECK (user_id != contact_id)
);

CREATE INDEX idx_contacts_user_id ON contacts(user_id);
CREATE INDEX idx_contacts_contact_id ON contacts(contact_id);

-- ============================================================================
-- CALLS TABLE (for history)
-- ============================================================================

CREATE TABLE IF NOT EXISTS calls (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    chat_id UUID NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    initiator_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- Call Info
    call_type VARCHAR(50) NOT NULL, -- 'voice', 'video'
    call_state VARCHAR(50) DEFAULT 'ringing', -- 'ringing', 'in_progress', 'missed', 'ended'
    duration_seconds INT,
    
    -- WebRTC Info
    ice_candidates TEXT,  -- Serialized STUN/TURN servers
    sdp_offer TEXT,
    
    -- Metadata
    started_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    ended_at TIMESTAMP WITH TIME ZONE,
    
    CONSTRAINT duration_positive CHECK (duration_seconds IS NULL OR duration_seconds >= 0)
);

CREATE INDEX idx_calls_chat_id ON calls(chat_id);
CREATE INDEX idx_calls_initiator_id ON calls(initiator_id);
CREATE INDEX idx_calls_state ON calls(call_state);

-- ============================================================================
-- ENCRYPTION KEYS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS encryption_keys (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    
    -- Key Data (encrypted in application layer)
    key_type VARCHAR(50) NOT NULL, -- 'dh', 'signing', 'backup'
    public_key TEXT NOT NULL,
    private_key_encrypted TEXT NOT NULL,  -- Encrypted with user's password
    private_key_iv VARCHAR(255),  -- IV for encryption
    
    -- Key Rotation
    is_active BOOLEAN DEFAULT true,
    revoked_at TIMESTAMP WITH TIME ZONE,
    
    -- Metadata
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP WITH TIME ZONE
);

CREATE INDEX idx_encryption_keys_user_id ON encryption_keys(user_id);
CREATE INDEX idx_encryption_keys_active ON encryption_keys(is_active);

-- ============================================================================
-- FILE UPLOADS TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS file_uploads (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    message_id UUID NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    uploader_id UUID NOT NULL REFERENCES users(id) ON DELETE SET NULL,
    
    -- File Info
    file_name VARCHAR(255) NOT NULL,
    file_size INT NOT NULL,
    file_type VARCHAR(100),  -- MIME type
    file_path TEXT NOT NULL,  -- S3/storage path
    
    -- Security
    is_encrypted BOOLEAN DEFAULT true,
    encryption_key_id UUID,
    
    -- Media-specific
    width INT,  -- For images/videos
    height INT,
    duration INT,  -- For videos/audio (seconds)
    
    -- Metadata
    uploaded_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    processed BOOLEAN DEFAULT false,
    
    CONSTRAINT file_size_positive CHECK (file_size > 0)
);

CREATE INDEX idx_file_uploads_message_id ON file_uploads(message_id);
CREATE INDEX idx_file_uploads_uploader_id ON file_uploads(uploader_id);

-- ============================================================================
-- AUDIT LOG TABLE
-- ============================================================================

CREATE TABLE IF NOT EXISTS audit_log (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    user_id UUID REFERENCES users(id) ON DELETE SET NULL,
    
    -- Event Info
    event_type VARCHAR(100) NOT NULL,
    event_data JSONB,  -- Flexible event data
    ip_address INET,
    user_agent TEXT,
    
    -- Metadata
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_audit_log_user_id ON audit_log(user_id);
CREATE INDEX idx_audit_log_event_type ON audit_log(event_type);
CREATE INDEX idx_audit_log_created_at ON audit_log(created_at DESC);

-- ============================================================================
-- TRIGGERS FOR UPDATED_AT COLUMNS
-- ============================================================================

CREATE OR REPLACE FUNCTION update_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER users_updated_at BEFORE UPDATE ON users
    FOR EACH ROW EXECUTE FUNCTION update_updated_at();

CREATE TRIGGER chats_updated_at BEFORE UPDATE ON chats
    FOR EACH ROW EXECUTE FUNCTION update_updated_at();

CREATE TRIGGER contacts_updated_at BEFORE UPDATE ON contacts
    FOR EACH ROW EXECUTE FUNCTION update_updated_at();

-- ============================================================================
-- VIEWS FOR COMMON QUERIES
-- ============================================================================

-- Active sessions view
CREATE OR REPLACE VIEW active_sessions AS
SELECT * FROM sessions
WHERE is_active = true AND expires_at > CURRENT_TIMESTAMP;

-- User presence view
CREATE OR REPLACE VIEW user_presence AS
SELECT u.id, u.username, u.status, u.last_seen,
       COUNT(s.id) as session_count
FROM users u
LEFT JOIN sessions s ON u.id = s.user_id AND s.is_active = true
GROUP BY u.id;

-- Unread messages view
CREATE OR REPLACE VIEW unread_messages AS
SELECT md.recipient_id, COUNT(*) as unread_count
FROM message_delivery md
WHERE md.status = 'delivered'
GROUP BY md.recipient_id;

-- ============================================================================
-- SAMPLE DATA (Optional - for testing)
-- ============================================================================

-- INSERT INTO users (username, email, password_hash, password_salt, first_name, public_key)
-- VALUES (
--     'testuser',
--     'test@example.com',
--     'hashed_password_here',
--     'salt_here',
--     'Test',
--     'e7f91c1a3...'
-- );

-- Timestamps and versioning
COMMENT ON TABLE users IS 'Core user accounts with E2E encryption keys';
COMMENT ON TABLE messages IS 'Messages (usually encrypted) with delivery status';
COMMENT ON TABLE message_delivery IS 'Per-recipient delivery status tracking';
COMMENT ON TABLE encryption_keys IS 'User encryption keys (private keys encrypted)';

-- ============================================================================
-- SECURITY NOTES
-- ============================================================================
/*
1. ENCRYPTION:
   - All sensitive data (passwords, private keys) encrypted at application layer
   - Messages stored encrypted in database
   - Decryption only happens client-side (Flutter app)

2. CONSTRAINTS:
   - Foreign keys prevent orphaned data
   - Check constraints validate data integrity
   - Unique constraints prevent duplicates

3. INDICES:
   - All join columns indexed for query performance
   - Created_at indexed for sorting/pagination
   - Status fields indexed for filtering

4. AUDIT:
   - audit_log tracks all important events
   - IP addresses recorded for security
   - Timestamps on all operations

5. ACCESS CONTROL:
   - Row-level security (RLS) can be added per tenant
   - PostgreSQL roles for different app layers
*/
