-- ============================================================================
-- Migration: add missing tables for HTTP API
-- Run this ONCE against your Railway PostgreSQL
-- ============================================================================

-- OTP codes (email-based login)
CREATE TABLE IF NOT EXISTS otp_codes (
    email       VARCHAR(255) PRIMARY KEY,
    code_hash   VARCHAR(128) NOT NULL,
    attempts    INT          NOT NULL DEFAULT 0,
    expires_at  TIMESTAMP WITH TIME ZONE NOT NULL,
    created_at  TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

-- Add display_name & status_text columns if missing from users
ALTER TABLE users ADD COLUMN IF NOT EXISTS display_name VARCHAR(100);
ALTER TABLE users ADD COLUMN IF NOT EXISTS status_text  VARCHAR(255);

-- Make password_hash / password_salt optional (OTP auth needs no password)
ALTER TABLE users ALTER COLUMN password_hash SET DEFAULT '';
ALTER TABLE users ALTER COLUMN password_salt SET DEFAULT '';

-- Make public_key optional (generated server-side for now)
ALTER TABLE users ALTER COLUMN public_key SET DEFAULT '';

-- Channels
CREATE TABLE IF NOT EXISTS channels (
    id               UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    username         VARCHAR(64)  UNIQUE NOT NULL,
    name             VARCHAR(255) NOT NULL,
    description      TEXT,
    avatar_url       TEXT,
    is_public        BOOLEAN      NOT NULL DEFAULT true,
    subscriber_count INT          NOT NULL DEFAULT 0,
    owner_id         UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at       TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_channels_username        ON channels(username);
CREATE INDEX IF NOT EXISTS idx_channels_is_public       ON channels(is_public);
CREATE INDEX IF NOT EXISTS idx_channels_subscriber_count ON channels(subscriber_count DESC);

-- Channel subscribers
CREATE TABLE IF NOT EXISTS channel_subscribers (
    id          UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    channel_id  UUID NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    user_id     UUID NOT NULL REFERENCES users(id)    ON DELETE CASCADE,
    role        VARCHAR(32) NOT NULL DEFAULT 'subscriber', -- owner, admin, subscriber
    joined_at   TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(channel_id, user_id)
);
CREATE INDEX IF NOT EXISTS idx_chan_subs_channel ON channel_subscribers(channel_id);
CREATE INDEX IF NOT EXISTS idx_chan_subs_user    ON channel_subscribers(user_id);

-- Channel posts
CREATE TABLE IF NOT EXISTS channel_posts (
    id          UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    channel_id  UUID NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    author_id   UUID REFERENCES users(id) ON DELETE SET NULL,
    content     TEXT NOT NULL,
    media_urls  TEXT[] DEFAULT '{}',
    is_paid     BOOLEAN NOT NULL DEFAULT false,
    created_at  TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_chan_posts_channel    ON channel_posts(channel_id);
CREATE INDEX IF NOT EXISTS idx_chan_posts_created_at ON channel_posts(created_at DESC);
