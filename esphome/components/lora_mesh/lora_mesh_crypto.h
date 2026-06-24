#pragma once

// AES-128-CCM payload encryption/decryption for lora_mesh per-Group security.
// Wire format: docs/wire-format.md §4.
//
// Nonce  (13 bytes): src_id(4) || dst_id(4) || frame_counter(4) || 0x00(1)
// AAD    (14 bytes): pkt_type(1) || src_id(4) || dst_id(4) || frame_counter(4) || payload_len(1)
// MIC    (4 bytes):  truncated CCM tag appended after ciphertext
//
// On embedded targets this uses mbedtls (already linked by ESP-IDF / Arduino).
// Host tests use a tiny reference AES-CCM built from first principles.

#include <cstdint>
#include <cstring>

namespace esphome::lora_mesh {

/// AES-128 key size in bytes.
static constexpr size_t GROUP_KEY_SIZE = 16;

/// MIC (Message Integrity Code) size in bytes — CCM tag truncated to 4 bytes.
static constexpr size_t MIC_SIZE = 4;

/// Nonce size for AES-128-CCM (13 bytes).
static constexpr size_t CCM_NONCE_SIZE = 13;

/// AAD size for the DATA packet (14 bytes).
static constexpr size_t CCM_AAD_SIZE = 14;

// ── Nonce / AAD construction ─────────────────────────────────────────────────

/// Build the 13-byte CCM nonce: src_id || dst_id || frame_counter || 0x00.
inline void build_ccm_nonce(uint8_t nonce[CCM_NONCE_SIZE], uint32_t src_id, uint32_t dst_id,
                            uint32_t frame_counter) {
  nonce[0] = static_cast<uint8_t>(src_id);
  nonce[1] = static_cast<uint8_t>(src_id >> 8);
  nonce[2] = static_cast<uint8_t>(src_id >> 16);
  nonce[3] = static_cast<uint8_t>(src_id >> 24);
  nonce[4] = static_cast<uint8_t>(dst_id);
  nonce[5] = static_cast<uint8_t>(dst_id >> 8);
  nonce[6] = static_cast<uint8_t>(dst_id >> 16);
  nonce[7] = static_cast<uint8_t>(dst_id >> 24);
  nonce[8] = static_cast<uint8_t>(frame_counter);
  nonce[9] = static_cast<uint8_t>(frame_counter >> 8);
  nonce[10] = static_cast<uint8_t>(frame_counter >> 16);
  nonce[11] = static_cast<uint8_t>(frame_counter >> 24);
  nonce[12] = 0x00;
}

/// Build the 14-byte AAD: pkt_type || src_id || dst_id || frame_counter || payload_len.
inline void build_ccm_aad(uint8_t aad[CCM_AAD_SIZE], uint8_t pkt_type, uint32_t src_id, uint32_t dst_id,
                          uint32_t frame_counter, uint8_t payload_len) {
  aad[0] = pkt_type;
  aad[1] = static_cast<uint8_t>(src_id);
  aad[2] = static_cast<uint8_t>(src_id >> 8);
  aad[3] = static_cast<uint8_t>(src_id >> 16);
  aad[4] = static_cast<uint8_t>(src_id >> 24);
  aad[5] = static_cast<uint8_t>(dst_id);
  aad[6] = static_cast<uint8_t>(dst_id >> 8);
  aad[7] = static_cast<uint8_t>(dst_id >> 16);
  aad[8] = static_cast<uint8_t>(dst_id >> 24);
  aad[9] = static_cast<uint8_t>(frame_counter);
  aad[10] = static_cast<uint8_t>(frame_counter >> 8);
  aad[11] = static_cast<uint8_t>(frame_counter >> 16);
  aad[12] = static_cast<uint8_t>(frame_counter >> 24);
  aad[13] = payload_len;
}

// ── Platform-specific AES-128-CCM implementation ─────────────────────────────

#if defined(USE_HOST) || defined(LORA_MESH_HOST_TEST)

// ─────────────────────────────────────────────────────────────────────────────
// Host / test: tiny reference AES-128-CCM (not optimised, correct only).
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// AES-128 block encrypt (single block, reference implementation).
inline void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  // Rijndael S-box
  static const uint8_t SBOX[256] = {
      0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
      0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
      0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
      0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
      0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
      0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
      0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
      0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
      0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
      0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
      0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
      0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
      0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
      0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
      0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
      0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
  };

  // xtime: GF(2^8) multiplication by x with AES irreducible polynomial 0x11b.
  auto xtime = [](uint8_t x) -> uint8_t { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); };

  uint8_t state[16];
  uint8_t rk[16];
  memcpy(state, in, 16);
  memcpy(rk, key, 16);

  // Initial AddRoundKey
  for (int i = 0; i < 16; i++) state[i] ^= rk[i];

  static const uint8_t RCON[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

  for (int round = 0; round < 10; round++) {
    // Key schedule: expand rk in-place
    uint8_t temp[4] = {rk[13], rk[14], rk[15], rk[12]};
    for (int i = 0; i < 4; i++) temp[i] = SBOX[temp[i]];
    temp[0] ^= RCON[round];
    for (int i = 0; i < 4; i++) rk[i] ^= temp[i];
    for (int i = 4; i < 16; i++) rk[i] ^= rk[i - 4];

    // SubBytes
    for (int i = 0; i < 16; i++) state[i] = SBOX[state[i]];

    // ShiftRows
    uint8_t t;
    t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    t = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = state[3]; state[3] = t;

    // MixColumns (skip on last round)
    if (round < 9) {
      for (int c = 0; c < 4; c++) {
        int i = c * 4;
        uint8_t a0 = state[i], a1 = state[i + 1], a2 = state[i + 2], a3 = state[i + 3];
        state[i] = static_cast<uint8_t>(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
        state[i + 1] = static_cast<uint8_t>(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
        state[i + 2] = static_cast<uint8_t>(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
        state[i + 3] = static_cast<uint8_t>(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
      }
    }

    // AddRoundKey
    for (int i = 0; i < 16; i++) state[i] ^= rk[i];
  }

  memcpy(out, state, 16);
}

/// AES-128-CCM encrypt (reference, for host tests).
/// Returns true on success. `out` must have room for `plaintext_len + MIC_SIZE`.
inline bool aes128_ccm_encrypt(const uint8_t key[16], const uint8_t *nonce, size_t nonce_len,
                               const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
                               size_t plaintext_len, uint8_t *out, uint8_t *tag, size_t tag_len) {
  if (nonce_len != 13 || tag_len != 4) return false;

  // q = 15 - nonce_len = 2 (encodes message length in 2 bytes)
  const uint8_t q = 2;

  // ── CBC-MAC (authentication) ────────────────────────────────────────────
  uint8_t b[16];
  // Flags: Adata=1 (bit 6), t=(tag_len-2)/2 = 1 (bits 5..3), q-1=1 (bits 2..0)
  b[0] = static_cast<uint8_t>(0x40 | (((tag_len - 2) / 2) << 3) | (q - 1));
  memcpy(b + 1, nonce, nonce_len);
  // Length of message in q bytes (big-endian)
  b[14] = static_cast<uint8_t>((plaintext_len >> 8) & 0xFF);
  b[15] = static_cast<uint8_t>(plaintext_len & 0xFF);

  uint8_t mac[16];
  aes128_encrypt_block(key, b, mac);

  // Process AAD (aad_len < 65280, so 2-byte length prefix)
  if (aad_len > 0) {
    uint8_t aad_block[16] = {};
    aad_block[0] = static_cast<uint8_t>((aad_len >> 8) & 0xFF);
    aad_block[1] = static_cast<uint8_t>(aad_len & 0xFF);
    size_t aad_copied = 0;
    size_t space = 14;  // 16 - 2 bytes for length prefix
    size_t copy_now = (aad_len < space) ? aad_len : space;
    memcpy(aad_block + 2, aad, copy_now);
    aad_copied = copy_now;
    for (int i = 0; i < 16; i++) mac[i] ^= aad_block[i];
    aes128_encrypt_block(key, mac, mac);

    while (aad_copied < aad_len) {
      uint8_t blk[16] = {};
      size_t remain = aad_len - aad_copied;
      size_t take = (remain < 16) ? remain : 16;
      memcpy(blk, aad + aad_copied, take);
      aad_copied += take;
      for (int i = 0; i < 16; i++) mac[i] ^= blk[i];
      aes128_encrypt_block(key, mac, mac);
    }
  }

  // Process plaintext blocks
  size_t offset = 0;
  while (offset < plaintext_len) {
    uint8_t blk[16] = {};
    size_t remain = plaintext_len - offset;
    size_t take = (remain < 16) ? remain : 16;
    memcpy(blk, plaintext + offset, take);
    offset += take;
    for (int i = 0; i < 16; i++) mac[i] ^= blk[i];
    aes128_encrypt_block(key, mac, mac);
  }

  // ── CTR mode encryption ─────────────────────────────────────────────────
  // A_0: flags=q-1, nonce, counter=0
  uint8_t ctr[16] = {};
  ctr[0] = q - 1;  // flags for counter block
  memcpy(ctr + 1, nonce, nonce_len);
  ctr[14] = 0;
  ctr[15] = 0;

  // Encrypt tag: S_0 = E(K, A_0)
  uint8_t s0[16];
  aes128_encrypt_block(key, ctr, s0);
  for (size_t i = 0; i < tag_len; i++) tag[i] = mac[i] ^ s0[i];

  // Encrypt plaintext: A_i for i=1,2,...
  offset = 0;
  uint16_t ctr_val = 1;
  while (offset < plaintext_len) {
    ctr[14] = static_cast<uint8_t>((ctr_val >> 8) & 0xFF);
    ctr[15] = static_cast<uint8_t>(ctr_val & 0xFF);
    uint8_t keystream[16];
    aes128_encrypt_block(key, ctr, keystream);
    size_t remain = plaintext_len - offset;
    size_t take = (remain < 16) ? remain : 16;
    for (size_t i = 0; i < take; i++) out[offset + i] = plaintext[offset + i] ^ keystream[i];
    offset += take;
    ctr_val++;
  }

  return true;
}

/// AES-128-CCM decrypt + verify (reference, for host tests).
/// Returns true if MIC verifies. `out` receives plaintext.
inline bool aes128_ccm_decrypt(const uint8_t key[16], const uint8_t *nonce, size_t nonce_len,
                               const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
                               size_t ciphertext_len, uint8_t *out, const uint8_t *tag, size_t tag_len) {
  if (nonce_len != 13 || tag_len != 4) return false;

  const uint8_t q = 2;

  // ── CTR mode decryption ─────────────────────────────────────────────────
  uint8_t ctr[16] = {};
  ctr[0] = q - 1;
  memcpy(ctr + 1, nonce, nonce_len);

  // Decrypt ciphertext
  size_t offset = 0;
  uint16_t ctr_val = 1;
  while (offset < ciphertext_len) {
    ctr[14] = static_cast<uint8_t>((ctr_val >> 8) & 0xFF);
    ctr[15] = static_cast<uint8_t>(ctr_val & 0xFF);
    uint8_t keystream[16];
    aes128_encrypt_block(key, ctr, keystream);
    size_t remain = ciphertext_len - offset;
    size_t take = (remain < 16) ? remain : 16;
    for (size_t i = 0; i < take; i++) out[offset + i] = ciphertext[offset + i] ^ keystream[i];
    offset += take;
    ctr_val++;
  }

  // ── CBC-MAC (verify) ────────────────────────────────────────────────────
  uint8_t b[16];
  b[0] = static_cast<uint8_t>(0x40 | (((tag_len - 2) / 2) << 3) | (q - 1));
  memcpy(b + 1, nonce, nonce_len);
  b[14] = static_cast<uint8_t>((ciphertext_len >> 8) & 0xFF);
  b[15] = static_cast<uint8_t>(ciphertext_len & 0xFF);

  uint8_t mac[16];
  aes128_encrypt_block(key, b, mac);

  if (aad_len > 0) {
    uint8_t aad_block[16] = {};
    aad_block[0] = static_cast<uint8_t>((aad_len >> 8) & 0xFF);
    aad_block[1] = static_cast<uint8_t>(aad_len & 0xFF);
    size_t aad_copied = 0;
    size_t space = 14;
    size_t copy_now = (aad_len < space) ? aad_len : space;
    memcpy(aad_block + 2, aad, copy_now);
    aad_copied = copy_now;
    for (int i = 0; i < 16; i++) mac[i] ^= aad_block[i];
    aes128_encrypt_block(key, mac, mac);

    while (aad_copied < aad_len) {
      uint8_t blk[16] = {};
      size_t remain = aad_len - aad_copied;
      size_t take = (remain < 16) ? remain : 16;
      memcpy(blk, aad + aad_copied, take);
      aad_copied += take;
      for (int i = 0; i < 16; i++) mac[i] ^= blk[i];
      aes128_encrypt_block(key, mac, mac);
    }
  }

  // MAC over decrypted plaintext
  offset = 0;
  while (offset < ciphertext_len) {
    uint8_t blk[16] = {};
    size_t remain = ciphertext_len - offset;
    size_t take = (remain < 16) ? remain : 16;
    memcpy(blk, out + offset, take);
    offset += take;
    for (int i = 0; i < 16; i++) mac[i] ^= blk[i];
    aes128_encrypt_block(key, mac, mac);
  }

  // S_0 for tag decryption
  ctr[14] = 0;
  ctr[15] = 0;
  uint8_t s0[16];
  aes128_encrypt_block(key, ctr, s0);

  // Compute expected tag
  uint8_t expected_tag[4];
  for (size_t i = 0; i < tag_len; i++) expected_tag[i] = mac[i] ^ s0[i];

  // Constant-time compare
  uint8_t diff = 0;
  for (size_t i = 0; i < tag_len; i++) diff |= expected_tag[i] ^ tag[i];

  if (diff != 0) {
    // MIC failed — clear output
    memset(out, 0, ciphertext_len);
    return false;
  }
  return true;
}

}  // namespace detail

/// Encrypt plaintext payload in-place for a DATA packet.
/// `payload` is input plaintext; on success, `out` contains ciphertext (same length as plaintext)
/// and `mic` contains the 4-byte MIC.
/// Returns true on success.
inline bool mesh_encrypt_payload(const uint8_t key[GROUP_KEY_SIZE], uint32_t src_id, uint32_t dst_id,
                                 uint32_t frame_counter, uint8_t pkt_type, uint8_t payload_len,
                                 const uint8_t *plaintext, uint8_t *ciphertext, uint8_t mic[MIC_SIZE]) {
  uint8_t nonce[CCM_NONCE_SIZE];
  uint8_t aad[CCM_AAD_SIZE];
  build_ccm_nonce(nonce, src_id, dst_id, frame_counter);
  build_ccm_aad(aad, pkt_type, src_id, dst_id, frame_counter, payload_len);
  return detail::aes128_ccm_encrypt(key, nonce, CCM_NONCE_SIZE, aad, CCM_AAD_SIZE, plaintext, payload_len, ciphertext,
                                    mic, MIC_SIZE);
}

/// Decrypt and verify a DATA packet payload.
/// `ciphertext` is the encrypted payload; `mic` is the 4-byte MIC from the wire.
/// On success, `plaintext` contains the decrypted payload.
/// Returns true if MIC verifies (packet is authentic).
inline bool mesh_decrypt_payload(const uint8_t key[GROUP_KEY_SIZE], uint32_t src_id, uint32_t dst_id,
                                 uint32_t frame_counter, uint8_t pkt_type, uint8_t payload_len,
                                 const uint8_t *ciphertext, uint8_t *plaintext, const uint8_t *mic) {
  uint8_t nonce[CCM_NONCE_SIZE];
  uint8_t aad[CCM_AAD_SIZE];
  build_ccm_nonce(nonce, src_id, dst_id, frame_counter);
  build_ccm_aad(aad, pkt_type, src_id, dst_id, frame_counter, payload_len);
  return detail::aes128_ccm_decrypt(key, nonce, CCM_NONCE_SIZE, aad, CCM_AAD_SIZE, ciphertext, payload_len, plaintext,
                                    mic, MIC_SIZE);
}

#else  // Embedded target — use mbedtls

#include "mbedtls/ccm.h"

inline bool mesh_encrypt_payload(const uint8_t key[GROUP_KEY_SIZE], uint32_t src_id, uint32_t dst_id,
                                 uint32_t frame_counter, uint8_t pkt_type, uint8_t payload_len,
                                 const uint8_t *plaintext, uint8_t *ciphertext, uint8_t mic[MIC_SIZE]) {
  uint8_t nonce[CCM_NONCE_SIZE];
  uint8_t aad[CCM_AAD_SIZE];
  build_ccm_nonce(nonce, src_id, dst_id, frame_counter);
  build_ccm_aad(aad, pkt_type, src_id, dst_id, frame_counter, payload_len);

  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (ret != 0) {
    mbedtls_ccm_free(&ctx);
    return false;
  }
  ret = mbedtls_ccm_encrypt_and_tag(&ctx, payload_len, nonce, CCM_NONCE_SIZE, aad, CCM_AAD_SIZE, plaintext, ciphertext,
                                    mic, MIC_SIZE);
  mbedtls_ccm_free(&ctx);
  return ret == 0;
}

inline bool mesh_decrypt_payload(const uint8_t key[GROUP_KEY_SIZE], uint32_t src_id, uint32_t dst_id,
                                 uint32_t frame_counter, uint8_t pkt_type, uint8_t payload_len,
                                 const uint8_t *ciphertext, uint8_t *plaintext, const uint8_t *mic) {
  uint8_t nonce[CCM_NONCE_SIZE];
  uint8_t aad[CCM_AAD_SIZE];
  build_ccm_nonce(nonce, src_id, dst_id, frame_counter);
  build_ccm_aad(aad, pkt_type, src_id, dst_id, frame_counter, payload_len);

  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (ret != 0) {
    mbedtls_ccm_free(&ctx);
    return false;
  }
  ret = mbedtls_ccm_auth_decrypt(&ctx, payload_len, nonce, CCM_NONCE_SIZE, aad, CCM_AAD_SIZE, ciphertext, plaintext,
                                 mic, MIC_SIZE);
  mbedtls_ccm_free(&ctx);
  return ret == 0;
}

#endif  // USE_HOST

}  // namespace esphome::lora_mesh
