#pragma once
#include <string>
#include <memory>
#include <vector>
#include <filesystem>

struct YSMProfileEntry {
	std::string stage;
	double milliseconds = 0.0;
};

class YSMParser {
public:
	// Disable copy and move semantics
	YSMParser() = default;
	virtual int getYSGPVersion() = 0;
	virtual void parse() = 0;
	virtual std::vector<uint8_t> getDecryptedData() = 0;
	virtual void saveToDirectory(std::string output_directory) = 0;
	void setVerbose(bool verbose) { m_verbose = verbose; }
	void setDebug(bool debug) { m_debug = debug; }
	void setFormatJson(bool formatJson) { m_formatJson = formatJson; }
	void setProfile(bool profile) { m_profile = profile; }
	const std::vector<YSMProfileEntry>& profileEntries() const { return m_profileEntries; }
	virtual ~YSMParser() = default;
protected:
	bool isVerbose() const { return m_verbose; }
	bool isDebug() const { return m_debug; }
	bool isFormatJson() const { return m_formatJson; }
	bool isProfile() const { return m_profile; }
	void clearProfileEntries() { m_profileEntries.clear(); }
	void recordProfileStage(const std::string& stage, double milliseconds) {
		if (m_profile) {
			m_profileEntries.push_back({ stage, milliseconds });
		}
	}
private:
	bool m_verbose = false;
	bool m_debug = false;
	bool m_formatJson = false;
	bool m_profile = false;
	std::vector<YSMProfileEntry> m_profileEntries;
};

namespace YSMParserFactory {
	std::unique_ptr<YSMParser> Create(const std::string& path);
	std::unique_ptr<YSMParser> Create(const char* data, size_t size);
}

namespace PathUtils {
    inline std::filesystem::path utf8_to_path(const std::string& str) {
#ifdef __cpp_char8_t
        auto u8str = std::u8string(str.begin(), str.end());
        return std::filesystem::path(u8str);
#else
        return std::filesystem::u8path(str);
#endif
    }
    
    inline std::string path_to_utf8(const std::filesystem::path& p) {
        auto native_path = p;
        native_path.make_preferred();
#ifdef __cpp_char8_t
        auto u8str = native_path.u8string();
        return std::string(u8str.begin(), u8str.end());
#else
        return native_path.u8string();
#endif
    }
}

namespace MemoryUtils {
	template<typename T>
	T readLE(const char* buffer, size_t offset = 0) {
		T value = 0;
		const unsigned char* ubuf = reinterpret_cast<const unsigned char*>(buffer + offset);
		for (size_t i = 0; i < sizeof(T); ++i) {
			value |= (static_cast<T>(ubuf[i]) << (8 * i));
		}
		return value;
	}

	template<typename T>
	T readBE(const char* buffer, size_t offset = 0) {
		T value = 0;
		const unsigned char* ubuf = reinterpret_cast<const unsigned char*>(buffer + offset);
		for (size_t i = 0; i < sizeof(T); ++i) {
			value = (value << 8) | static_cast<T>(ubuf[i]);
		}
		return value;
	}

	/**
	 * 将数值以小端序写入缓冲区
	 * @param buffer 目标缓冲区
	 * @param value 要写入的数值
	 * @param offset 偏移量
	 */
	template<typename T>
	void writeLE(char* buffer, T value, size_t offset = 0) {
		unsigned char* ubuf = reinterpret_cast<unsigned char*>(buffer + offset);
		for (size_t i = 0; i < sizeof(T); ++i) {
			// 通过右移 8*i 位并取低 8 位，获取每一字节
			ubuf[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
		}
	}

	/**
	 * 针对 24 位整数（常用在某些协议长度字段）的特化写入
	 */
	inline void writeLE24(char* buffer, uint32_t value, size_t offset = 0) {
		unsigned char* ubuf = reinterpret_cast<unsigned char*>(buffer + offset);
		ubuf[0] = static_cast<unsigned char>(value & 0xFF);
		ubuf[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
		ubuf[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
	}

	inline uint32_t readLE24(const char* buffer, size_t offset = 0) {
		const auto* ubuf = reinterpret_cast<const unsigned char*>(buffer + offset);
		return static_cast<uint32_t>(ubuf[0]) |
			static_cast<uint32_t>(ubuf[1]) << 8 |
			static_cast<uint32_t>(ubuf[2]) << 16;
	}

	inline std::string readStr(const char* buffer, size_t offset, size_t size) {
		const char* strStart = buffer + offset;
		return std::string(strStart, size);
	}
}
