#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <array>

/**
 * @class E2EEncryption
 * @brief End-to-End Encryption module using libsodium
 * 
 * Implements:
 * - X25519 Elliptic Curve Diffie-Hellman (ECDH)
 * - ChaCha20-Poly1305 AEAD cipher
 * - BLAKE3 / HMAC-SHA256 for authentication
 * - Double Ratchet for forward secrecy (like Signal)
 */
class E2EEncryption {
public:
    // Key sizes (in bytes)
    static constexpr size_t PUBLIC_KEY_SIZE = 32;    // X25519
    static constexpr size_t PRIVATE_KEY_SIZE = 32;   // X25519
    static constexpr size_t SHARED_SECRET_SIZE = 32; // DH result
    static constexpr size_t NONCE_SIZE = 12;         // ChaCha20-Poly1305
    static constexpr size_t CIPHER_KEY_SIZE = 32;    // 256-bit
    static constexpr size_t AUTH_TAG_SIZE = 16;      // Poly1305 MAC
    static constexpr size_t SIGNATURE_SIZE = 64;     // Ed25519

    /**
     * @struct KeyPair
     * Asymmetric key pair for ECDH
     */
    struct KeyPair {
        std::array<uint8_t, PUBLIC_KEY_SIZE> public_key;
        std::array<uint8_t, PRIVATE_KEY_SIZE> private_key;

        std::string public_key_hex() const;
        static KeyPair from_hex(const std::string& public_hex, const std::string& private_hex);
    };

    /**
     * @struct EncryptedMessage
     * Encrypted message with metadata
     */
    struct EncryptedMessage {
        std::vector<uint8_t> ciphertext;
        std::array<uint8_t, NONCE_SIZE> nonce;
        std::array<uint8_t, AUTH_TAG_SIZE> auth_tag;
        uint64_t message_id = 0;
        uint64_t timestamp = 0;
        
        std::vector<uint8_t> serialize() const;
        static EncryptedMessage deserialize(const std::vector<uint8_t>& data);
    };

    /**
     * @struct SessionKeys
     * Derived keys for a conversation
     */
    struct SessionKeys {
        std::array<uint8_t, CIPHER_KEY_SIZE> send_key;
        std::array<uint8_t, CIPHER_KEY_SIZE> recv_key;
        std::array<uint8_t, 32> header_key;  // For sender/receiver info
        uint64_t send_counter = 0;
        uint64_t recv_counter = 0;
    };

    // Initialization & Key Generation
    static bool initialize();  // Initialize libsodium
    static KeyPair generate_keypair();
    static std::array<uint8_t, SIGNATURE_SIZE> sign_message(
        const std::vector<uint8_t>& message,
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& signing_key
    );
    static bool verify_signature(
        const std::vector<uint8_t>& message,
        const std::array<uint8_t, SIGNATURE_SIZE>& signature,
        const std::array<uint8_t, PUBLIC_KEY_SIZE>& verifying_key
    );

    // ECDH Key Exchange
    static std::array<uint8_t, SHARED_SECRET_SIZE> compute_shared_secret(
        const std::array<uint8_t, PRIVATE_KEY_SIZE>& private_key,
        const std::array<uint8_t, PUBLIC_KEY_SIZE>& peer_public_key
    );

    // Session Establishment
    static SessionKeys derive_session_keys(
        const std::array<uint8_t, SHARED_SECRET_SIZE>& shared_secret,
        const std::string& salt = ""
    );

    // Encryption / Decryption
    static EncryptedMessage encrypt(
        const std::vector<uint8_t>& plaintext,
        const std::array<uint8_t, CIPHER_KEY_SIZE>& key,
        uint64_t message_counter
    );

    static std::vector<uint8_t> decrypt(
        const EncryptedMessage& encrypted,
        const std::array<uint8_t, CIPHER_KEY_SIZE>& key,
        uint64_t expected_counter
    );

    // Double Ratchet (forward secrecy - simplified version)
    class DoubleRatchet {
    public:
        DoubleRatchet() = default;
        explicit DoubleRatchet(const std::array<uint8_t, SHARED_SECRET_SIZE>& initial_secret);

        // Perform ratchet step (generates new keys, advances counter)
        SessionKeys ratchet_forward();
        
        // Get current keys without advancing
        SessionKeys get_current_keys() const;

        // Update with peer's public key (ratchet forward in DH)
        void update_with_peer_key(const std::array<uint8_t, PUBLIC_KEY_SIZE>& peer_key);

    private:
        std::array<uint8_t, SHARED_SECRET_SIZE> root_key_;
        std::array<uint8_t, SHARED_SECRET_SIZE> dh_send_private_;
        std::array<uint8_t, PUBLIC_KEY_SIZE> dh_send_public_;
        std::array<uint8_t, PUBLIC_KEY_SIZE> dh_recv_public_;
        uint32_t chain_counter_ = 0;
    };

    // Utilities
    static std::string bytes_to_hex(const std::vector<uint8_t>& bytes);
    static std::vector<uint8_t> hex_to_bytes(const std::string& hex);
    static std::vector<uint8_t> derive_key(
        const std::string& password,
        const std::vector<uint8_t>& salt,
        size_t key_length
    );

private:
    static bool sodium_initialized_;
};
