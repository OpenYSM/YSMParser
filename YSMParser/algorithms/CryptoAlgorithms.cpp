#include "CryptoAlgorithms.hpp"
#include <random>
#include <city.h>
#include <zstd.h>
#include "../parsers/YSMParser.hpp"
#include <array>
#include <limits>
#include <span>
#include <string>
#include "YsmZstd.hpp"

namespace CryptoUtils {

size_t xchacha_update_state(XChaCha_ctx* ctx, uint64_t hash_v) {
	ctx->rounds = 10 * (hash_v % 3) + 10;

	uint32_t lo = static_cast<uint32_t>(hash_v & 0xFFFFFFFF);
	uint32_t hi = static_cast<uint32_t>((hash_v >> 32) & 0xFFFFFFFF);

	for (int i = 4; i < 16; ++i) {
		if (i % 2 == 0) {
			ctx->input[i] ^= lo;
		}
		else {
			ctx->input[i] ^= hi;
		}
	}

	return static_cast<size_t>(((hash_v & 0x3F) | 0x40) << 6);
}

std::vector<uint8_t> ModifiedChaChaEncrypt(const std::vector<uint8_t>& data, const uint8_t* key, const uint8_t* iv, const uint64_t seed) {
	// 1. 初始状态与解密完全一致
	std::vector<uint8_t> key_iv(56);
	std::memcpy(key_iv.data(), key, 32);
	std::memcpy(key_iv.data() + 32, iv, 24);

	uint64_t hash2 = CityHash64WithSeed(reinterpret_cast<const char*>(key_iv.data()), key_iv.size(), seed);

	size_t next_round_size = ((hash2 & 0x3f) | 0x40) << 6;
	size_t blockPointer = 0;

	XChaCha_ctx ctx;
	ctx.rounds = 10 * (hash2 % 3) + 10; // 魔改点：根据 Hash 动态决定轮数 (10, 20 或 30 轮)
	xchacha_keysetup(&ctx, key, iv);

	std::vector<uint8_t> result;
	result.reserve(data.size());

	// 2. 加密循环
	while (blockPointer < data.size()) {
		if (blockPointer + next_round_size > data.size()) {
			next_round_size = data.size() - blockPointer;
		}

		// 这里的 data 是【明文】
		std::vector<uint8_t> plain1(data.begin() + blockPointer, data.begin() + blockPointer + next_round_size);
		blockPointer += next_round_size;

		std::vector<uint8_t> enc1(next_round_size);
		// 执行加密
		xchacha_encrypt_bytes(&ctx, plain1.data(), enc1.data(), (uint32_t)next_round_size);

		// 【关键点】：必须对【明文 plain1】进行 Hash，以保证与解密端状态同步更新
		uint64_t res_hash = CityHash64WithSeed(reinterpret_cast<const char*>(plain1.data()), next_round_size, seed);

		// 更新状态，获取下一个块的大小
		next_round_size = xchacha_update_state(&ctx, res_hash);

		result.insert(result.end(), enc1.begin(), enc1.end());
	}

	return result;
}

std::vector<uint8_t> ModifiedChaChaDecrypt(std::span<const uint8_t> data, const uint8_t* key, const uint8_t* iv, const uint64_t seed) {
	std::vector<uint8_t> key_iv(56);
	std::memcpy(key_iv.data(), key, 32);
	std::memcpy(key_iv.data() + 32, iv, 24);

	uint64_t hash2 = CityHash64WithSeed(reinterpret_cast<const char*>(key_iv.data()), key_iv.size(), seed);

	size_t next_round_size = ((hash2 & 0x3f) | 0x40) << 6;
	size_t blockPointer = 0;

	XChaCha_ctx ctx;
	ctx.rounds = 10 * (hash2 % 3) + 10;
	xchacha_keysetup(&ctx, key, iv);

	std::vector<uint8_t> result(data.size());

	while (blockPointer < data.size()) {
		if (blockPointer + next_round_size > data.size()) {
			next_round_size = data.size() - blockPointer;
		}

		const uint8_t* enc1 = data.data() + blockPointer;
		uint8_t* dec1 = result.data() + blockPointer;
		blockPointer += next_round_size;

		xchacha_decrypt_bytes(&ctx, enc1, dec1, (uint32_t)next_round_size);

		uint64_t res_hash = CityHash64WithSeed(reinterpret_cast<const char*>(dec1), next_round_size, seed);

		next_round_size = xchacha_update_state(&ctx, res_hash);
	}

	return result;
}

std::vector<uint8_t> ModifiedChaChaDecrypt(const std::vector<uint8_t>& data, const uint8_t* key, const uint8_t* iv, const uint64_t seed) {
	return ModifiedChaChaDecrypt(std::span<const uint8_t>(data.data(), data.size()), key, iv, seed);
}

void MT19937XorInPlace(std::span<uint8_t> data, const uint8_t* key, const uint8_t* iv) {
	std::vector<uint8_t> key_iv(56);
	std::memcpy(key_iv.data(), key, 32);
	std::memcpy(key_iv.data() + 32, iv, 24);

	uint64_t seed = CityHash64WithSeed(reinterpret_cast<const char*>(key_iv.data()), key_iv.size(), SEED_KEY_DERIVATION);

	std::mt19937_64 mt(seed);

	size_t i = 0;
	while (i < data.size()) {
		uint64_t rnd = mt();
		for (int j = 0; j < 8 && i < data.size(); ++j) {
			uint8_t keystream_byte = static_cast<uint8_t>((rnd >> (j * 8)) & 0xFF);
			data[i] ^= keystream_byte;
			++i;
		}
	}
}

std::vector<uint8_t> MT19937Xor_Decrypt(const std::vector<uint8_t>& data, const uint8_t* key, const uint8_t* iv) {
	std::vector<uint8_t> result = data;
	MT19937XorInPlace(std::span<uint8_t>(result.data(), result.size()), key, iv);

	return result;
}

std::vector<uint8_t> DecompressZstd(std::span<const uint8_t> compressed_data) {
	auto washed_data = YsmZstd::wash(compressed_data);
	unsigned long long const known_size = ZSTD_getFrameContentSize(washed_data.data(), washed_data.size());
	if (known_size == ZSTD_CONTENTSIZE_ERROR) {
		throw std::runtime_error("ZSTD decompression failed: invalid frame content size");
	}
	if (known_size != ZSTD_CONTENTSIZE_UNKNOWN) {
		if (known_size > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
			throw std::runtime_error("ZSTD decompression failed: output is too large");
		}
		std::vector<uint8_t> decompressed_data(static_cast<std::size_t>(known_size));
		size_t const ret = ZSTD_decompress(
			decompressed_data.data(),
			decompressed_data.size(),
			washed_data.data(),
			washed_data.size()
		);
		if (ZSTD_isError(ret)) {
			throw std::runtime_error(std::string("ZSTD decompression failed: ") + ZSTD_getErrorName(ret));
		}
		decompressed_data.resize(ret);
		return decompressed_data;
	}

	ZSTD_DCtx* dctx = ZSTD_createDCtx();
	if (dctx == nullptr) {
		throw std::runtime_error("Failed to create ZSTD decompression context!");
	}

	std::vector<uint8_t> decompressed_data;
	if (washed_data.size() <= std::numeric_limits<std::size_t>::max() / 3) {
		decompressed_data.reserve(washed_data.size() * 3);
	}

	ZSTD_inBuffer input = { washed_data.data(), washed_data.size(), 0 };

	size_t const outBuffSize = ZSTD_DStreamOutSize();
	std::vector<uint8_t> outBuff(outBuffSize);

	while (input.pos < input.size) {
		ZSTD_outBuffer output = { outBuff.data(), outBuff.size(), 0 };

		size_t const ret = ZSTD_decompressStream(dctx, &output, &input);

		if (ZSTD_isError(ret)) {
			ZSTD_freeDCtx(dctx);
			throw std::runtime_error(std::string("ZSTD decompression failed: ") + ZSTD_getErrorName(ret));
		}

		decompressed_data.insert(decompressed_data.end(), outBuff.begin(), outBuff.begin() + output.pos);
	}

	ZSTD_freeDCtx(dctx);

	return decompressed_data;
}

std::vector<uint8_t> DecompressZstd(const std::vector<uint8_t>& compressed_data) {
	return DecompressZstd(std::span<const uint8_t>(compressed_data.data(), compressed_data.size()));
}

std::vector<uint8_t> CompressZstd(const std::vector<uint8_t>& data, int level) {
	// 获取压缩后所需的最大缓冲大小
	size_t const dstBound = ZSTD_compressBound(data.size());
	std::vector<uint8_t> compressed_data(dstBound);

	// 标准压缩
	size_t const cSize = ZSTD_compress(
		compressed_data.data(), compressed_data.size(),
		data.data(), data.size(),
		level
	);

	if (ZSTD_isError(cSize)) {
		throw std::runtime_error(std::string("ZSTD compression failed: ") + ZSTD_getErrorName(cSize));
	}

	// 裁剪掉多余的 buffer 空间
	compressed_data.resize(cSize);

	// 混淆为 YSM 魔改格式并返回
	return YsmZstd::obfuscate(compressed_data);
}

std::vector<uint8_t> EncryptPacket(const std::vector<uint8_t>& data, std::vector<uint8_t>& Key, std::vector<uint8_t>& nextKey)
{
	std::vector<uint8_t> full_data = data;

	XChaCha_ctx ctx;
	ctx.rounds = 30;
	xchacha_keysetup(&ctx, (const uint8_t*)Key.data(), (const uint8_t*)(Key.data() + 0x20));
	xchacha_encrypt_bytes(&ctx, (const uint8_t*)full_data.data(), full_data.data(), full_data.size());
	full_data = CryptoUtils::MT19937Xor_Decrypt(full_data, (const uint8_t*)Key.data(), (const uint8_t*)(Key.data() + 0x20));

	/* Derive New Key */
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<uint64_t> dis;
	nextKey.resize(56);
	MemoryUtils::writeLE<uint64_t>(reinterpret_cast<char*>(nextKey.data()), dis(gen), 0x0);
	MemoryUtils::writeLE<uint64_t>(reinterpret_cast<char*>(nextKey.data()), dis(gen), 0x8);
	MemoryUtils::writeLE<uint64_t>(reinterpret_cast<char*>(nextKey.data()), dis(gen), 0x10);
	MemoryUtils::writeLE<uint64_t>(reinterpret_cast<char*>(nextKey.data()), dis(gen), 0x18);
	MemoryUtils::writeLE<uint64_t>(reinterpret_cast<char*>(nextKey.data()), dis(gen), 0x20);
	MemoryUtils::writeLE<uint64_t>(reinterpret_cast<char*>(nextKey.data()), dis(gen), 0x28);
	MemoryUtils::writeLE<uint64_t>(reinterpret_cast<char*>(nextKey.data()), dis(gen), 0x30);

	full_data.insert(full_data.end(), nextKey.begin(), nextKey.end());
	uint64_t hash = CityHash64WithSeed(reinterpret_cast<const char*>(full_data.data()), full_data.size(), SEED_PACKET_VERIFICATION);

	size_t original_size = full_data.size();
	full_data.resize(original_size + 8);
	MemoryUtils::writeLE<uint64_t>(reinterpret_cast<char*>(full_data.data() + original_size), hash);

	return full_data;
}

static int HexNibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

std::pair<uint64_t, uint64_t> DeriveHashFromFileName(const std::string& fileName, const std::vector<uint8_t>& rtKey)
{
	if (rtKey.size() != 56) return {};
	std::pair<uint64_t, uint64_t> out{};

	if (fileName.size() != 40) {
		return out;
	}

	std::array<std::uint8_t, 20> buf{  };
	for (size_t i = 0; i < 20; ++i) {
		int hi = HexNibble(fileName[i * 2]);
		int lo = HexNibble(fileName[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			return {};
		}
		buf[i] = static_cast<std::uint8_t>((hi << 4) | lo);
	}for (size_t i = 0; i < buf.size(); ++i) {
		buf[i] ^= rtKey[i % rtKey.size()];

	}
	const std::uint32_t seed = MemoryUtils::readLE<uint32_t>(reinterpret_cast<const char*>(buf.data()));
	std::mt19937_64 mt(static_cast<std::uint64_t>(seed));

	out.first = MemoryUtils::readLE<uint64_t>(reinterpret_cast<const char*>(buf.data()), 4) ^ mt();
	out.second = MemoryUtils::readLE<uint64_t>(reinterpret_cast<const char*>(buf.data()), 4 + 8) ^ mt();
	return out;
}

std::vector<uint8_t> DecryptCachedModel(const std::vector<uint8_t>& fileData, const std::pair<uint64_t, uint64_t>& fileHash, const std::vector<uint8_t>& rtKey)
{
	uint64_t verif = CityHash64WithSeed(reinterpret_cast<const char*>(fileData.data()), fileData.size() - 8, SEED_CACHE_VERIFICATION);
	verif ^= fileHash.first;
	verif ^= fileHash.second;

	auto realHash = MemoryUtils::readLE<uint64_t>(reinterpret_cast<const char*>(fileData.data()), fileData.size() - 8);
	if (verif != realHash) return std::vector<uint8_t>();

	BufferReader reader = { fileData.data(), fileData.size(), 0 };

	if (reader.readVarint() != 1) throw ParserUnknownField();
	if (reader.readVarint() != 0) throw ParserUnknownField();
	if (reader.readVarint() != 0) throw ParserUnknownField();
	if (reader.readVarint() != 0) throw ParserUnknownField();

	uint64_t format = reader.readVarint();
	if (reader.readVarint() != 0) throw ParserUnknownField();
	if (reader.readVarint() != 0) throw ParserUnknownField();
	if (reader.readVarint() != 0) throw ParserUnknownField();
	if (reader.readVarint() != 0) throw ParserUnknownField();

	auto dataIn = reader.readBytesExactly(reader.size - reader.offset - 8);

	auto data = ModifiedChaChaDecrypt(dataIn, rtKey.data(), rtKey.data() + 0x20, SEED_CACHE_DECRYPTION);
	std::vector<uint8_t> xorred_data = MT19937Xor_Decrypt(data, rtKey.data(), rtKey.data() + 0x20);

	uint16_t n = static_cast<uint16_t>(xorred_data[0]) | (static_cast<uint16_t>(xorred_data[1]) << 8);
	n &= 0x3ff;
	data.assign(xorred_data.begin() + 2 + n, xorred_data.end());
	return data;
}

std::vector<uint8_t> VerifyAndDecryptPacket(const std::vector<uint8_t>& packet, const std::vector<uint8_t>& Key)
{
	if (packet.size() <= 2 + 1 + 8) {
		return {};
	}
	uint64_t hash = CityHash64WithSeed(reinterpret_cast<const char*>(packet.data()), packet.size() - 8, SEED_PACKET_VERIFICATION);
	uint64_t packet_hash = MemoryUtils::readLE<uint64_t>(reinterpret_cast<const char*>(packet.data()), packet.size() - 8);
	if (hash != packet_hash) {
		return {};
	}

	XChaCha_ctx ctx{};
	ctx.rounds = 30;
	xchacha_keysetup(&ctx, (const uint8_t*)Key.data(), (const uint8_t*)(Key.data() + 0x20));

	std::vector<uint8_t> data;
	data.assign(packet.begin(), packet.end() - 8);
	data = CryptoUtils::MT19937Xor_Decrypt(data, (const uint8_t*)Key.data(), (const uint8_t*)(Key.data() + 0x20));
	xchacha_decrypt_bytes(&ctx, (const uint8_t*)data.data(), data.data(), data.size());

	BufferReader reader{ data.data(), data.size(), 0 };
	uint8_t length = reader.readWordLE() & 0x7f;
	reader.offset = 2 + length;
	uint64_t type = reader.readVarint();
	switch (type)
	{
	case 1:
	{
		return reader.readBytesExactly(0x38);
	}
	case 2:
	{
		uint8_t unk = reader.readVarint();
		return reader.readBytesExactly(0x38);
	}
	case 3:
	{
		uint64_t unk1 = reader.readVarint();
		reader.offset += 0x1c;
		reader.offset += 0x1c;

		auto modelKey = reader.readBytesExactly(0x38);

		uint64_t len = reader.readVarint();
		for (uint64_t i = 0; i < len; i++)
		{
			uint64_t unk = reader.readVarint();
		}

		for (uint64_t i = 0; i < len; i++)
		{
			std::string uName = reader.readString();
			if (reader.readVarint() != 0) throw ParserUnknownField();
			if (reader.readVarint() != 0) throw ParserUnknownField();

			uint64_t format = reader.readVarint();
			reader.readVarint();
			reader.readVarint();
		}
		return modelKey;
	}
	case 4:
	{
		uint64_t unk = reader.readVarint();
		break;
	}
	default:
		break;
	}

	return {};
}

}
