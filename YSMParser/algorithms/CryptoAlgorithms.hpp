#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <string>
#include <utility>
#include <xchacha20.h>
#include "../parsers/BufferReader.hpp"

constexpr uint64_t SEED_KEY_DERIVATION = 0xD017CBBA7B5D3581;
constexpr uint64_t SEED_RES_VERIFICATION = 0xA62B1A2C43842BC3;
constexpr uint64_t SEED_CACHE_DECRYPTION = 0xD1C3D1D13A99752B;
constexpr uint64_t SEED_FILE_VERIFICATION = 0x9E5599DB80C67C29;
constexpr uint64_t SEED_PACKET_VERIFICATION = 0xEE6FA63D570BD77B;
constexpr uint64_t SEED_CACHE_VERIFICATION = 0xF346451E53A22261;

namespace CryptoUtils {

size_t xchacha_update_state(XChaCha_ctx* ctx, uint64_t hash_v);

std::vector<uint8_t> ModifiedChaChaDecrypt(std::span<const uint8_t> data, const uint8_t* key, const uint8_t* iv, uint64_t seed);

std::vector<uint8_t> ModifiedChaChaDecrypt(const std::vector<uint8_t>& data, const uint8_t* key, const uint8_t* iv, uint64_t seed);

std::vector<uint8_t> ModifiedChaChaEncrypt(const std::vector<uint8_t>& data, const uint8_t* key, const uint8_t* iv, const uint64_t seed);

void MT19937XorInPlace(std::span<uint8_t> data, const uint8_t* key, const uint8_t* iv);

std::vector<uint8_t> MT19937Xor_Decrypt(const std::vector<uint8_t>& data, const uint8_t* key, const uint8_t* iv);

std::vector<uint8_t> DecompressZstd(std::span<const uint8_t> compressed_data);

std::vector<uint8_t> DecompressZstd(const std::vector<uint8_t>& compressed_data);

std::vector<uint8_t> CompressZstd(const std::vector<uint8_t>& data, int level);

std::vector<uint8_t> EncryptPacket(const std::vector<uint8_t>& data, std::vector<uint8_t>& Key, std::vector<uint8_t>& nextKey);

std::pair<uint64_t, uint64_t> DeriveHashFromFileName(const std::string& fileName, const std::vector<uint8_t>& rtKey);

std::vector<uint8_t> DecryptCachedModel(const std::vector<uint8_t>& fileData, const std::pair<uint64_t, uint64_t>& fileHash, const std::vector<uint8_t>& rtKey);

std::vector<uint8_t> VerifyAndDecryptPacket(const std::vector<uint8_t>& packet, const std::vector<uint8_t>& Key);

}
