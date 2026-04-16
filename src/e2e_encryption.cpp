#include "e2e_encryption.hpp"
#include <sodium.h>
#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <cstring>

bool E2EEncryption::sodium_initialized_ = false;

bool E2EEncryption::initialize() {
    if (sodium_initialized_) return true;
    
    if (sodium_init() < 0) {
        throw std::runtime_error("Failed to initialize libsodium");
    }
    
    sodium_initialized_ = true;
    return true;
}

E2EEncryption::KeyPair E2EEncryption::generate_keypair() {
    if (!sodium_initialized_) initialize();
    
    KeyPair kp;
    unsigned char pk[PUBLIC_KEY_SIZE];
    unsigned char sk[PRIVATE_KEY_SIZE];
    
    crypto_kx_keypair(pk, sk);
    
    std::memcpy(kp.public_key.data(), pk, PUBLIC_KEY_SIZE);
    std::memcpy(kp.private_key.data(), sk, PRIVATE_KEY_SIZE);
    
    return kp;
}

std::array<uint8_t, E2EEncryption::SIGNATURE_SIZE> E2EEncryption::sign_message(
    const std::vector<uint8_t>& message,
    const std::array<uint8_t, PRIVATE_KEY_SIZE>& signing_key) {
    
    if (!sodium_initialized_) initialize();
    
    std::array<uint8_t, SIGNATURE_SIZE> signature;
    unsigned long long sig_len;
    
    crypto_sign_detached(
        signature.data(),
        &sig_len,
        message.data(),
        message.size(),
        signing_key.data()
    );
    
    return signature;
}

bool E2EEncryption::verify_signature(
    const std::vector<uint8_t>& message,
    const std::array<uint8_t, SIGNATURE_SIZE>& signature,
    const std::array<uint8_t, PUBLIC_KEY_SIZE>& verifying_key) {
    
    if (!sodium_initialized_) initialize();
    
    return crypto_sign_verify_detached(
        signature.data(),
        message.data(),
        message.size(),
        verifying_key.data()
    ) == 0;
}

std::array<uint8_t, E2EEncryption::SHARED_SECRET_SIZE> E2EEncryption::compute_shared_secret(
    const std::array<uint8_t, PRIVATE_KEY_SIZE>& private_key,
    const std::array<uint8_t, PUBLIC_KEY_SIZE>& peer_public_key) {
    
    if (!sodium_initialized_) initialize();
    
    std::array<uint8_t, SHARED_SECRET_SIZE> shared_secret;
    unsigned char rx[32], tx[32];
    unsigned char own_pk[32];
    crypto_scalarmult_base(own_pk, private_key.data());
    
    if (crypto_kx_client_session_keys(
        rx, tx,
        own_pk,
        private_key.data(),
        peer_public_key.data()
    ) != 0) {
        throw std::runtime_error("Shared secret computation failed");
    }
    
    std::memcpy(shared_secret.data(), rx, SHARED_SECRET_SIZE);
    return shared_secret;
}

E2EEncryption::SessionKeys E2EEncryption::derive_session_keys(
    const std::array<uint8_t, SHARED_SECRET_SIZE>& shared_secret,
    const std::string& salt) {
    
    if (!sodium_initialized_) initialize();
    
    SessionKeys keys{};
    
    // Use HKDF-SHA256 for key derivation
    unsigned char salt_bytes[16] = {0};
    
    if (!salt.empty()) {
        size_t salt_len = std::min(salt.length(), size_t(16));
        std::memcpy(salt_bytes, salt.c_str(), salt_len);
    }
    
    // Derive send_key
    if (crypto_pwhash(
        keys.send_key.data(), CIPHER_KEY_SIZE,
        (const char*)shared_secret.data(), SHARED_SECRET_SIZE,
        salt_bytes,
        crypto_pwhash_OPSLIMIT_SENSITIVE,
        crypto_pwhash_MEMLIMIT_SENSITIVE,
        crypto_pwhash_ALG_DEFAULT
    ) != 0) {
        throw std::runtime_error("Key derivation failed");
    }
    
    // Derive recv_key (different salt component)
    unsigned char modified_salt[16];
    std::memcpy(modified_salt, salt_bytes, 16);
    modified_salt[0] ^= 0xFF;  // Flip one byte to make it different
    
    if (crypto_pwhash(
        keys.recv_key.data(), CIPHER_KEY_SIZE,
        (const char*)shared_secret.data(), SHARED_SECRET_SIZE,
        modified_salt,
        crypto_pwhash_OPSLIMIT_SENSITIVE,
        crypto_pwhash_MEMLIMIT_SENSITIVE,
        crypto_pwhash_ALG_DEFAULT
    ) != 0) {
        throw std::runtime_error("Key derivation failed");
    }
    
    return keys;
}

E2EEncryption::EncryptedMessage E2EEncryption::encrypt(
    const std::vector<uint8_t>& plaintext,
    const std::array<uint8_t, CIPHER_KEY_SIZE>& key,
    uint64_t message_counter) {
    
    if (!sodium_initialized_) initialize();
    
    EncryptedMessage encrypted;
    encrypted.timestamp = std::time(nullptr);
    encrypted.message_id = message_counter;
    
    // Generate nonce from counter (12 bytes)
    unsigned char nonce_bytes[NONCE_SIZE];
    std::memcpy(nonce_bytes, &message_counter, 8);
    randombytes_buf(nonce_bytes + 8, 4);  // Random 4 bytes
    std::memcpy(encrypted.nonce.data(), nonce_bytes, NONCE_SIZE);
    
    // Allocate ciphertext buffer (plaintext + auth tag)
    encrypted.ciphertext.resize(plaintext.size() + AUTH_TAG_SIZE);
    
    // Encrypt using ChaCha20-Poly1305
    crypto_aead_chacha20poly1305_encrypt(
        encrypted.ciphertext.data(),
        nullptr,
        plaintext.data(),
        plaintext.size(),
        nullptr,  // No additional data
        0,
        nullptr,  // No secret nonce
        nonce_bytes,
        key.data()
    );
    
    // Extract auth tag (last 16 bytes)
    std::memcpy(
        encrypted.auth_tag.data(),
        encrypted.ciphertext.data() + encrypted.ciphertext.size() - AUTH_TAG_SIZE,
        AUTH_TAG_SIZE
    );
    
    // Remove auth tag from ciphertext (it'll be verified during decryption)
    encrypted.ciphertext.resize(encrypted.ciphertext.size() - AUTH_TAG_SIZE);
    
    return encrypted;
}

std::vector<uint8_t> E2EEncryption::decrypt(
    const EncryptedMessage& encrypted,
    const std::array<uint8_t, CIPHER_KEY_SIZE>& key,
    uint64_t /*expected_counter*/) {
    
    if (!sodium_initialized_) initialize();
    
    // Prepare buffer for decryption (with auth tag appended)
    std::vector<uint8_t> ciphertext_with_tag = encrypted.ciphertext;
    ciphertext_with_tag.insert(
        ciphertext_with_tag.end(),
        encrypted.auth_tag.begin(),
        encrypted.auth_tag.end()
    );
    
    std::vector<uint8_t> plaintext(ciphertext_with_tag.size() - AUTH_TAG_SIZE);
    
    int result = crypto_aead_chacha20poly1305_decrypt(
        plaintext.data(),
        nullptr,
        nullptr,  // Don't validate tag separately
        ciphertext_with_tag.data(),
        ciphertext_with_tag.size(),
        nullptr,  // No additional data
        0,
        encrypted.nonce.data(),
        key.data()
    );
    
    if (result != 0) {
        throw std::runtime_error("Decryption failed - authentication tag verification failed");
    }
    
    return plaintext;
}

std::string E2EEncryption::bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    return oss.str();
}

std::vector<uint8_t> E2EEncryption::hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

std::vector<uint8_t> E2EEncryption::derive_key(
    const std::string& password,
    const std::vector<uint8_t>& salt,
    size_t key_length) {
    
    if (!sodium_initialized_) initialize();
    
    std::vector<uint8_t> derived_key(key_length);
    
    unsigned char salt_bytes[16];
    std::memcpy(salt_bytes, salt.data(), std::min(salt.size(), size_t(16)));
    
    if (crypto_pwhash(
        derived_key.data(),
        key_length,
        password.c_str(),
        password.length(),
        salt_bytes,
        crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE,
        crypto_pwhash_ALG_DEFAULT
    ) != 0) {
        throw std::runtime_error("Key derivation failed");
    }
    
    return derived_key;
}

std::string E2EEncryption::KeyPair::public_key_hex() const {
    return bytes_to_hex(std::vector<uint8_t>(public_key.begin(), public_key.end()));
}

E2EEncryption::KeyPair E2EEncryption::KeyPair::from_hex(
    const std::string& public_hex,
    const std::string& private_hex) {
    
    KeyPair kp;
    auto pub_bytes = hex_to_bytes(public_hex);
    auto priv_bytes = hex_to_bytes(private_hex);
    
    std::memcpy(kp.public_key.data(), pub_bytes.data(), std::min(pub_bytes.size(), PUBLIC_KEY_SIZE));
    std::memcpy(kp.private_key.data(), priv_bytes.data(), std::min(priv_bytes.size(), PRIVATE_KEY_SIZE));
    
    return kp;
}

std::vector<uint8_t> E2EEncryption::EncryptedMessage::serialize() const {
    std::vector<uint8_t> result;
    
    // Format: [message_id(8)] [timestamp(8)] [nonce(12)] [ciphertext_len(4)] [ciphertext] [auth_tag(16)]
    
    result.insert(result.end(), (uint8_t*)&message_id, (uint8_t*)&message_id + 8);
    result.insert(result.end(), (uint8_t*)&timestamp, (uint8_t*)&timestamp + 8);
    result.insert(result.end(), nonce.begin(), nonce.end());
    
    uint32_t cipher_len = ciphertext.size();
    result.insert(result.end(), (uint8_t*)&cipher_len, (uint8_t*)&cipher_len + 4);
    result.insert(result.end(), ciphertext.begin(), ciphertext.end());
    result.insert(result.end(), auth_tag.begin(), auth_tag.end());
    
    return result;
}

E2EEncryption::EncryptedMessage E2EEncryption::EncryptedMessage::deserialize(
    const std::vector<uint8_t>& data) {
    
    EncryptedMessage msg;
    
    if (data.size() < 32) {
        throw std::runtime_error("Invalid serialized message format");
    }
    
    size_t offset = 0;
    
    std::memcpy(&msg.message_id, data.data() + offset, 8);
    offset += 8;
    
    std::memcpy(&msg.timestamp, data.data() + offset, 8);
    offset += 8;
    
    std::memcpy(msg.nonce.data(), data.data() + offset, NONCE_SIZE);
    offset += NONCE_SIZE;
    
    uint32_t cipher_len;
    std::memcpy(&cipher_len, data.data() + offset, 4);
    offset += 4;
    
    msg.ciphertext.insert(
        msg.ciphertext.end(),
        data.begin() + offset,
        data.begin() + offset + cipher_len
    );
    offset += cipher_len;
    
    std::memcpy(msg.auth_tag.data(), data.data() + offset, AUTH_TAG_SIZE);
    
    return msg;
}

// DoubleRatchet Implementation

E2EEncryption::DoubleRatchet::DoubleRatchet(
    const std::array<uint8_t, SHARED_SECRET_SIZE>& initial_secret) 
    : root_key_(initial_secret) {
    
    // Initialize DH keys
    auto kp = generate_keypair();
    std::memcpy(dh_send_public_.data(), kp.public_key.data(), PUBLIC_KEY_SIZE);
    std::memcpy(dh_send_private_.data(), kp.private_key.data(), PRIVATE_KEY_SIZE);
}

E2EEncryption::SessionKeys E2EEncryption::DoubleRatchet::ratchet_forward() {
    // Increment counter
    chain_counter_++;
    
    // Derive new root key and message key
    std::vector<uint8_t> root_key_vec(root_key_.begin(), root_key_.end());
    std::vector<uint8_t> new_root = derive_key("root_ratchet", root_key_vec, 32);
    std::memcpy(root_key_.data(), new_root.data(), 32);
    
    // Derive session keys
    return derive_session_keys(root_key_);
}

E2EEncryption::SessionKeys E2EEncryption::DoubleRatchet::get_current_keys() const {
    return derive_session_keys(root_key_);
}

void E2EEncryption::DoubleRatchet::update_with_peer_key(
    const std::array<uint8_t, PUBLIC_KEY_SIZE>& peer_key) {
    
    // DH ratchet: compute new shared secret with peer's key
    std::array<uint8_t, SHARED_SECRET_SIZE> new_secret = compute_shared_secret(
        dh_send_private_,
        peer_key
    );
    
    // Update root key
    std::vector<uint8_t> new_root_vec = derive_key(
        "dh_ratchet",
        std::vector<uint8_t>(new_secret.begin(), new_secret.end()),
        32
    );
    std::memcpy(root_key_.data(), new_root_vec.data(), 32);
    
    // Store peer's public key
    std::memcpy(dh_recv_public_.data(), peer_key.data(), PUBLIC_KEY_SIZE);
    
    // Reset chain counter
    chain_counter_ = 0;
}
