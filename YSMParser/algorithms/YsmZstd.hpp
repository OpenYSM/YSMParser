#include <vector>
#include <cstdint>
#include <stdexcept>
#include <span>

class YsmZstd {
private:
    static constexpr uint32_t STD_BT_RAW = 0;
    static constexpr uint32_t STD_BT_RLE = 1;
    static constexpr uint32_t STD_BT_COMPRESSED = 2;
    static constexpr uint32_t STD_BT_RESERVED = 3;

    /**
     * 复用之前的解析逻辑，计算 Frame Header 长度
     */
    static int calculateFrameHeaderSize(uint8_t fhd) {
        int size = 1; // FHD 本身占 1 字节

        int fcsFieldSize = fhd & 3; // bits 0-1
        bool singleSegment = ((fhd >> 5) & 1) == 1; // bit 5

        int dictIdSize = 0;
        int dictIdBits = fhd & 3;
        if (dictIdBits == 1) dictIdSize = 1;
        else if (dictIdBits == 2) dictIdSize = 2;
        else if (dictIdBits == 3) dictIdSize = 4;

        int fcsSize = 0;
        int fcsBits = (fhd >> 6) & 3;
        if (fcsBits == 0) fcsSize = singleSegment ? 1 : 0;
        else if (fcsBits == 1) fcsSize = 2;
        else if (fcsBits == 2) fcsSize = 4;
        else if (fcsBits == 3) fcsSize = 8;

        int windowDescSize = singleSegment ? 0 : 1;

        return size + windowDescSize + dictIdSize + fcsSize;
    }

public:
    /**
     * 洗白操作：将 YSM 魔改的 ZSTD 恢复为标准格式
     */
    static std::vector<uint8_t> wash(std::span<const uint8_t> compressed_data) {
        if (compressed_data.size() < 5) {
            throw std::invalid_argument("Invalid data length");
        }

        // 拷贝一份数据用于原地修改并返回
        std::vector<uint8_t> data(compressed_data.begin(), compressed_data.end());

        // 1. 验证 ZSTD Magic Number (0xFD2FB528) - 小端读取
        uint32_t magic = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        if (magic != 0xFD2FB528) {
            throw std::invalid_argument("Not a standard ZSTD Magic Number. May be skippable frame or unknown.");
        }

        // 2. 擦除 Frame Header 中的 Checksum 标志位 (绕过 XXHash 魔改)
        uint8_t fhd = data[4];
        data[4] = fhd & 0xFB;

        // 3. 计算 Frame Header 的长度，从而找到第一个 Block 的起点
        int frameHeaderSize = calculateFrameHeaderSize(fhd);
        size_t offset = 4 + frameHeaderSize;

        // 4. 遍历并洗白所有 Block Header
        while (offset + 3 <= data.size()) {
            uint32_t b0 = data[offset];
            uint32_t b1 = data[offset + 1];
            uint32_t b2 = data[offset + 2];

            // --- 提取 YSM 魔改字段 ---
            uint32_t lastBlock = (b0 >> 7) & 1;
            uint32_t blockTypeYSM = (b0 >> 5) & 3;

            uint32_t rawSize = ((b0 & 0x1F) << 16) | b1 | (b2 << 8);
            uint32_t cSize = rawSize ^ 0xD4E9;

            // --- 映射回官方 Standard Block Type ---
            uint32_t blockTypeStd;
            switch (blockTypeYSM) {
            case 0: blockTypeStd = STD_BT_COMPRESSED; break;
            case 1: blockTypeStd = STD_BT_RLE; break;
            case 2: blockTypeStd = STD_BT_RESERVED; break;
            case 3: blockTypeStd = STD_BT_RAW; break;
            default: throw std::logic_error("Unknown block type");
            }

            // --- 重新组装为官方 Standard Block Header ---
            uint32_t stdHeader = lastBlock | (blockTypeStd << 1) | (cSize << 3);

            data[offset] = stdHeader & 0xFF;
            data[offset + 1] = (stdHeader >> 8) & 0xFF;
            data[offset + 2] = (stdHeader >> 16) & 0xFF;

            // 移动到下一个 block
            uint32_t blockDataSize = (blockTypeStd == STD_BT_RLE) ? 1 : cSize;
            offset += 3 + blockDataSize;

            if (lastBlock == 1) {
                break; // 这是最后一个块，跳出
            }
        }

        return data;
    }

    static std::vector<uint8_t> wash(const std::vector<uint8_t>& compressed_data) {
        return wash(std::span<const uint8_t>(compressed_data.data(), compressed_data.size()));
    }

    /**
     * 将标准 ZSTD 数据“弄脏”为 YSM 魔改格式
     */
    static std::vector<uint8_t> obfuscate(const std::vector<uint8_t>& compressed_data) {
        if (compressed_data.size() < 5) {
            throw std::invalid_argument("Invalid data length");
        }

        // 拷贝一份数据用于原地修改并返回
        std::vector<uint8_t> data = compressed_data;

        // 1. 验证是否为标准 ZSTD Magic Number
        uint32_t magic = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        if (magic != 0xFD2FB528) {
            throw std::invalid_argument("Not a standard ZSTD frame.");
        }

        // 2. 读取 FHD，确认 Frame Header 大小以找到第一个 Block
        uint8_t fhd = data[4];
        int frameHeaderSize = calculateFrameHeaderSize(fhd);
        size_t offset = 4 + frameHeaderSize;

        // 3. 遍历并混淆所有 Block Header
        while (offset + 3 <= data.size()) {
            uint32_t b0 = data[offset];
            uint32_t b1 = data[offset + 1];
            uint32_t b2 = data[offset + 2];
            uint32_t cBlockHeader = b0 | (b1 << 8) | (b2 << 16);

            // --- 提取标准字段 ---
            uint32_t lastBlock = cBlockHeader & 1;
            uint32_t blockTypeStd = (cBlockHeader >> 1) & 3;
            uint32_t cSize = cBlockHeader >> 3;

            // 移动到下一个 block 的偏移量准备
            uint32_t blockDataSize = (blockTypeStd == STD_BT_RLE) ? 1 : cSize;

            // --- 映射为 YSM Block Type ---
            uint32_t blockTypeYSM;
            switch (blockTypeStd) {
            case STD_BT_RAW: blockTypeYSM = 3; break;
            case STD_BT_RLE: blockTypeYSM = 1; break;
            case STD_BT_COMPRESSED: blockTypeYSM = 0; break;
            case STD_BT_RESERVED: blockTypeYSM = 2; break;
            default: throw std::logic_error("Unknown block type");
            }

            // --- YSM Size 异或加密 ---
            uint32_t rawSize = cSize ^ 0xD4E9;

            // --- 重新组装为 YSM 魔改 Block Header ---
            uint32_t ysmB0 = (lastBlock << 7) | (blockTypeYSM << 5) | ((rawSize >> 16) & 0x1F);
            uint32_t ysmB1 = rawSize & 0xFF;
            uint32_t ysmB2 = (rawSize >> 8) & 0xFF;

            // 写回
            data[offset] = static_cast<uint8_t>(ysmB0);
            data[offset + 1] = static_cast<uint8_t>(ysmB1);
            data[offset + 2] = static_cast<uint8_t>(ysmB2);

            offset += 3 + blockDataSize;

            if (lastBlock == 1) {
                break; // 最后一个块，跳出
            }
        }

        return data;
    }
};
