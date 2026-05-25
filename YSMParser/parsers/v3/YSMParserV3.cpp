#include "YSMParserV3.hpp"
#include "../exceptions/ParserException.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include "../YSMParser.hpp"
#include <city.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <variant>
#include <string.h>
#include <array>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include <json.hpp>
#include <set>
#include <cmath>
#include <span>

#include <fpng.h>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include "../../platform/PlatformCompat.hpp"
#include "../BufferReader.hpp"
#include "../../algorithms/CryptoAlgorithms.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef YSM_ENABLE_V3_TRACE_PRINTF
static int ysm_trace_printf(const char*, ...) { return 0; }
#define printf(...) ysm_trace_printf(__VA_ARGS__)
#endif

class ScopedOstreamStateSilencer {
public:
	ScopedOstreamStateSilencer(std::ostream& stream, bool enabled)
		: stream_(stream), oldState_(stream.rdstate()), enabled_(enabled) {
		if (enabled_) {
			stream_.setstate(std::ios_base::badbit);
		}
	}

	~ScopedOstreamStateSilencer() {
		if (enabled_) {
			stream_.clear(oldState_);
		}
	}

private:
	std::ostream& stream_;
	std::ios::iostate oldState_;
	bool enabled_;
};

template <typename JsonType>
static void clean_json_floats(JsonType& j) {
	if (j.is_number_float()) {
		double val = j.template get<double>();
		j = std::round(val * 100000.0) / 100000.0;
	}
	else if (j.is_object() || j.is_array()) {
		for (auto& element : j) {
			clean_json_floats(element);
		}
	}
}
/**
 * @brief 将 Windows 文件名中的非法字符替换为指定字符
 *
 * 非法字符包括: \ / : * ? " < > |
 */
static std::string sanitizeWindowsFilename(std::string filename, char replacement = '_') {
	// 1. Windows 不允许的 9 个非法字符
	const std::string invalidChars = "\\/:*?\"<>|";

	// 2. 遍历并替换非法字符
	for (char& c : filename) {
		if (invalidChars.find(c) != std::string::npos) {
			c = replacement;
		}
	}

	// 3. 处理 Windows 的特殊限制：文件名末尾不能是空格或句点
	// 使用 erase-remove 思想，但这里通常只是去掉尾部的无效字符
	while (!filename.empty() && (filename.back() == ' ' || filename.back() == '.')) {
		filename.pop_back();
	}

	// 4. 如果文件名被删空了（例如原始输入是 "..."），给一个默认名
	if (filename.empty()) {
		return "unnamed_file";
	}

	return filename;
}

namespace Images {
	// 1=BMP, 2=PNG, 3=JPEG, 4=WEBP, 5=AVIF
	inline bool isKnownImageFormat(uint32_t formatCode) {
		return formatCode >= 1 && formatCode <= 5;
	}

	// 统一的图像元数据异常校验与输出函数
	inline static void validateImageMetadata(const std::string& name, size_t offset, uint32_t imageFormat, uint32_t unkFlag) {
		if (!isKnownImageFormat(imageFormat) || unkFlag != 1) {
			fprintf(stderr, "[Warning] Unexpected image metadata for '%s' at 0x%08zX: format=%u, unk_flag=%u\n",
				name.c_str(), offset, imageFormat, unkFlag);
		}
	}

	inline static std::string indent_multiline(std::string text, const std::string& indent) {
		std::string result;
		result.reserve(text.size() + indent.size() * 4);

		bool at_line_start = true;
		for (char ch : text) {
			if (at_line_start) {
				result += indent;
				at_line_start = false;
			}
			result.push_back(ch);
			if (ch == '\n') {
				at_line_start = true;
			}
		}

		return result;
	}

}

std::vector<uint8_t> YSMParserV3::getDecryptedData()
{
	return m_decompressed;
}

[[nodiscard]] static std::vector<uint8_t> encodeRgbaToPngMemory(const std::vector<uint8_t>& rgbaData, int w, int h) {
	if (rgbaData.size() != static_cast<size_t>(w * h * 4)) return {};

	std::vector<uint8_t> pngBytes;

	if (!fpng::fpng_encode_image_to_memory(rgbaData.data(), w, h, 4, pngBytes)) {
		throw ParserUnknownField();
	}

	return pngBytes;
}

[[nodiscard]] static std::vector<uint8_t> bytesFromString(const std::string& text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

static void appendFile(std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, std::string name, std::vector<uint8_t> data) {
	files.emplace_back(std::move(name), std::move(data));
}

[[nodiscard]] static const char* animationNameForModelId(uint32_t modelId, bool useExpandedNames) {
	switch (modelId) {
	case 1: return "main";
	case 2: return "arm";
	case 3: return "extra";
	case 4: return "tac";
	case 5: return "arrow";
	case 6: return "carryon";
	case 7: return "parcool";
	case 8: return "swem";
	case 9: return "slashblade";
	case 10: return "tlm";
	case 11: return useExpandedNames ? "fp_arm" : "fp.arm";
	case 12: return "immersive_melodies";
	case 13: return useExpandedNames ? "irons_spell_books" : "iss";
	default: throw ParserUnknownField();
	}
}

[[nodiscard]] static std::string formatKeyframeTime(float time) {
	if (std::abs(time) < 1e-6f) {
		return "0.0";
	}

	char buffer[64];
	const int written = std::snprintf(buffer, sizeof(buffer), "%.6f", static_cast<double>(time));
	if (written <= 0) {
		return "0.0";
	}

	std::string result(buffer, static_cast<std::size_t>(std::min<int>(written, sizeof(buffer) - 1)));
	while (!result.empty() && result.back() == '0') {
		result.pop_back();
	}
	if (!result.empty() && result.back() == '.') {
		result.push_back('0');
	}
	return result;
}


static int extractFormatFromHeader(const std::string& headerData) {
	size_t pos = headerData.find("<format>");
	if (pos == std::string::npos) throw std::runtime_error("no <format> label");
	pos += 8;
	while (pos < headerData.size() && (headerData[pos] == ' ' || headerData[pos] == '\t'))
		pos++;
	size_t end = pos;
	while (end < headerData.size() && headerData[end] >= '0' && headerData[end] <= '9')
		end++;
	if (end == pos) throw std::runtime_error("failed to parse <format>");
	return std::stoi(headerData.substr(pos, end - pos));
}

// 数据结构
// 差值模式映射
enum class LerpMode : uint8_t {
	LINEAR = 0,
	STEP = 1,
	CATMULLROM = 2
};

enum class LoopMode : uint8_t {
	ONCE = 0,
	LOOP = 1,
	UNK2 = 2,
	HOLD_ON_LAST_FRAME = 3,
};

static const char* lerp_mode_to_string(LerpMode mode) {
	switch (mode) {
	case LerpMode::LINEAR:
		return "linear";
	case LerpMode::STEP:
		return "step";
	case LerpMode::CATMULLROM:
		return "catmullrom";
	default:
		return "unknown";
	}
}

using MolangValue = std::variant<float, std::string>;


struct MolangPair {
	std::array<MolangValue, 3> m;
};

static std::ostream& operator<<(std::ostream& os, const MolangPair& pair) {
	os << "[";
	for (size_t i = 0; i < 3; ++i) {
		// std::visit 会根据 variant 的当前类型调用对应的操作
		std::visit([&os](auto&& arg) {
			os << arg;
			}, pair.m[i]);

		if (i < 2) os << ", ";
	}
	os << "]";
	return os;
}

struct TimeLine {
	std::vector<std::pair<float, std::vector<std::string>>> times;
};


struct Effects {
	std::vector<std::pair<float, std::string>> effects;
};


// Rotation -> Position -> Scale
struct Bone {
	std::string name;
	std::optional<MolangPair> rotation;
	std::optional<MolangPair> position;
	std::optional<MolangPair> scale;
};

struct Keyframe {
	MolangPair post;
	std::optional<MolangPair> pre;
	LerpMode lerp_mode = LerpMode::LINEAR;
};

struct BonesKeyFrame {
	std::vector<std::pair<float, Keyframe>> bone;
};

// 动画剪辑
struct AnimationClip {
	std::string name;
	float animation_length; // 80.0f 或 +INF
	std::vector<Bone> bones;
};


static std::optional<BonesKeyFrame> parseChannel(BufferReader& reader) {
	uint32_t molangs = reader.readVarint();

	if (molangs == 0x00) {
		return std::nullopt; // 占位符，说明该通道不存在
	}

	BonesKeyFrame kf;
	kf.bone.reserve(molangs);
	for (uint32_t i = 0; i < molangs; i++)
	{
		Keyframe frame;
		float time = reader.readFloat() / 20;
		frame.lerp_mode = static_cast<LerpMode>(reader.readVarint());

		// 先读取第一组数据
		MolangPair first_data;
		for (int j = 0; j < 3; j++)
		{
			uint8_t type = reader.readByte();
			if (type == 0x01) {
				first_data.m[j] = reader.readFloat();
			}
			else if (type == 0x02) {
				first_data.m[j] = reader.readString();
			}
			else {
				printf("ERROR AT 0x%02zX\n", reader.offset);
				throw std::runtime_error("Unknown keyframe type: " + std::to_string(type));
			}
		}

		size_t hasPre = reader.readVarint();
		if (hasPre >= 2) throw ParserUnknownField(); // 目前只见过 0 和 1 两种情况，其他值不合理

		if (hasPre)
		{
			// 如果还有额外数据（pre），读取第二组数据
			MolangPair second_data;
			for (int j = 0; j < 3; j++)
			{
				uint8_t type = reader.readByte();
				if (type == 0x01) {
					second_data.m[j] = reader.readFloat();
				}
				else if (type == 0x02) {
					second_data.m[j] = reader.readString(); // 此处修复了原代码少处理 String 模式的隐患
				}
				else {
					throw std::runtime_error("Unknown keyframe type: " + std::to_string(type));
				}
			}

			// 在这里进行反转，将先读的设为 pre，后读的设为 post
			frame.pre = std::move(first_data);
			frame.post = std::move(second_data);
		}
		else
		{
			// 如果没有额外数据，第一组数据即为 post
			frame.post = std::move(first_data);
		}
		kf.bone.emplace_back(time, std::move(frame));
	}

	return kf;
}


static std::optional<Effects> parseEffect(BufferReader& reader) {
	uint32_t header = reader.readVarint();

	if (header == 0x00) {
		return std::nullopt;
	}

	Effects eff;
	eff.effects.reserve(header);
	for (uint32_t i = 0; i < header; i++)
	{
		std::string effect = reader.readString();
		float time = reader.readFloat() / 20;
		eff.effects.emplace_back(time, std::move(effect));
	}

	return eff;
}


static std::optional<TimeLine> parseTimeLine(BufferReader& reader) {
	uint32_t header = reader.readVarint();

	if (header == 0x00) {
		return std::nullopt;
	}

	TimeLine timeline;
	timeline.times.reserve(header);
	for (uint32_t i = 0; i < header; i++)
	{
		uint32_t tl_inside = reader.readVarint();
		std::vector<std::string> events;
		events.reserve(tl_inside);
		for (uint32_t j = 0; j < tl_inside; j++)
		{
			events.emplace_back(reader.readString());
		}
		float time = reader.readFloat() / 20;
		timeline.times.emplace_back(time, std::move(events));
	}

	return timeline;
}

typedef struct {
	Vector3D vec;
	float u, v;
} Vertex;

typedef struct {
	Vector3D normal; // 法线 normal
	Vertex vertices[4];
} Face;

// --- 数学辅助函数 ---
static float dot(const Vector3D& a, const Vector3D& b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vector3D cross(const Vector3D& a, const Vector3D& b) {
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

static float length(const Vector3D& a) {
	return std::sqrt(dot(a, a));
}

static Vector3D normalize(const Vector3D& a) {
	float l = length(a);
	return l > 1e-8f ? a / l : Vector3D{ 0, 0, 0 };
}

static Vector3D round_vec(const Vector3D& a) {
	return { std::round(a.x), std::round(a.y), std::round(a.z) };
}

struct Matrix3x3 {
	Vector3D col[3];

	float det() const {
		return dot(col[0], cross(col[1], col[2]));
	}

	// 矩阵转置乘以向量 (相当于 mat.T @ v)
	Vector3D transpose_mul(const Vector3D& v) const {
		return { dot(col[0], v), dot(col[1], v), dot(col[2], v) };
	}
};

// 提取欧拉角
// 对应矩阵: P' = R_z(z) * R_y(y) * R_x(x) * P
static Vector3D matrix_to_euler_xyz(const Matrix3x3& m) {
	// m.col[0].x -> R00
	// m.col[0].y -> R10
	// m.col[0].z -> R20
	// m.col[1].z -> R21
	// m.col[2].z -> R22
	float sy = std::sqrt(m.col[0].x * m.col[0].x + m.col[0].y * m.col[0].y);
	bool singular = sy < 1e-6;
	float x_rad, y_rad, z_rad;

	if (!singular) {
		x_rad = std::atan2(m.col[1].z, m.col[2].z);
		y_rad = std::atan2(-m.col[0].z, sy);
		z_rad = std::atan2(m.col[0].y, m.col[0].x);
	}
	else {
		// 万向节死锁情况下的退化处理
		x_rad = std::atan2(-m.col[2].y, m.col[1].y);
		y_rad = std::atan2(-m.col[0].z, sy);
		z_rad = 0;
	}

	// 弧度转角度
	float rad_to_deg = static_cast<float>(180.0 / M_PI);
	return { x_rad * rad_to_deg, y_rad * rad_to_deg, z_rad * rad_to_deg };
}

struct UVBox {
	float u, v;
	float u_size, v_size;
};

struct BlockbenchCube {
	Vector3D origin;
	Vector3D size;
	float inflate;
	Vector3D pivot;
	Vector3D rotation;
	std::map<std::string, UVBox> uv;
};

static void get_expected_uv_dirs(Vector3D local_normal, Vector3D& exp_U, Vector3D& exp_V) {
	Vector3D n = round_vec(local_normal);
	exp_U = { 0, 0, 0 }; exp_V = { 0, 0, 0 };
	if (n.x == -1) { exp_U = { 0, 0, 1 };  exp_V = { 0, -1, 0 }; return; }
	if (n.x == 1) { exp_U = { 0, 0, -1 }; exp_V = { 0, -1, 0 }; return; }
	if (n.y == 1) { exp_U = { -1, 0, 0 }; exp_V = { 0, 0, -1 }; return; }
	if (n.y == -1) { exp_U = { -1, 0, 0 }; exp_V = { 0, 0, 1 }; return; }
	if (n.z == -1) { exp_U = { -1, 0, 0 }; exp_V = { 0, -1, 0 }; return; }
	if (n.z == 1) { exp_U = { 1, 0, 0 };  exp_V = { 0, -1, 0 }; return; }
}

struct FaceInfo {
	Vector3D n_w;
	Vector3D T_u;
	Vector3D T_v;
	const Face* raw_f;
};

static float clean_val(float v) {
	return std::round(v * 10000.0f) / 10000.0f;
}

static BlockbenchCube restore_blockbench_cube(const std::vector<Face>& faces_data, float original_inflate = 0.0f, int texture_width = 512, int texture_height = 512) {
	std::set<Vector3D> unique_pts;

	std::vector<Vector3D> candidate_axes;
	std::vector<FaceInfo> face_infos;

	for (const auto& f : faces_data) {
		Vector3D verts[4];
		for (int i = 0; i < 4; i++) {
			verts[i] = f.vertices[i].vec * 16.0f;
			Vector3D p4 = { std::round(verts[i].x * 10000.f) / 10000.f,
							std::round(verts[i].y * 10000.f) / 10000.f,
							std::round(verts[i].z * 10000.f) / 10000.f };
			unique_pts.insert(p4);
		}

		Vector3D n_w = f.normal;
		Vector3D T_u = { 0,0,0 }, T_v = { 0,0,0 };

		for (int i = 0; i < 4; i++) {
			int j = (i + 1) % 4; // 获取下一个相邻顶点
			Vector3D dx = verts[i] - verts[j];
			float du = f.vertices[i].u - f.vertices[j].u;
			float dv = f.vertices[i].v - f.vertices[j].v;
			float len_dx = length(dx);

			// 过滤掉距离过近的重合顶点，避免除以零
			if (len_dx < 1e-5f) continue;

			if (std::abs(du) > 1e-5f && std::abs(dv) < 1e-5f) {
				T_u = (dx / len_dx) * (du > 0 ? 1.0f : -1.0f);
			}
			if (std::abs(dv) > 1e-5f && std::abs(du) < 1e-5f) {
				T_v = (dx / len_dx) * (dv > 0 ? 1.0f : -1.0f);
			}
		}

		face_infos.push_back({ n_w, T_u, T_v, &f });

		candidate_axes.push_back(n_w);
		if (length(T_u) > 0.5f) candidate_axes.push_back(T_u);
		if (length(T_v) > 0.5f) candidate_axes.push_back(T_v);
	}

	std::vector<Vector3D> raw_axes;
	for (const auto& vec : candidate_axes) {
		Vector3D vec_norm = normalize(vec);
		bool axis_exists = false;
		for (auto a : raw_axes) {
			if (std::abs(dot(vec_norm, a)) > 0.95f) { axis_exists = true; break; }
		}
		if (!axis_exists) raw_axes.push_back(vec_norm);
		if (raw_axes.size() == 3) break;
	}

	if (raw_axes.size() == 1) {
		Vector3D n = raw_axes[0];
		Vector3D temp = std::abs(n.x) < 0.9f ? Vector3D{ 1,0,0 } : Vector3D{ 0,1,0 };
		Vector3D u = normalize(cross(n, temp));
		raw_axes.push_back(u); raw_axes.push_back(normalize(cross(n, u)));
	}
	else if (raw_axes.size() == 2) {
		Vector3D w = cross(raw_axes[0], raw_axes[1]);
		if (length(w) > 1e-5f) raw_axes.push_back(normalize(w));
	}
	else if (raw_axes.size() == 0) {
		raw_axes = { {1,0,0}, {0,1,0}, {0,0,1} };
	}

	float best_score = -std::numeric_limits<float>::infinity();
	Matrix3x3 best_rot_matrix;
	Vector3D best_euler = { 0,0,0 };
	float min_euler_sum = std::numeric_limits<float>::infinity();

	int perms[6][3] = { {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0} };
	float signs[8][3] = { {1,1,1}, {1,1,-1}, {1,-1,1}, {1,-1,-1}, {-1,1,1}, {-1,1,-1}, {-1,-1,1}, {-1,-1,-1} };

	for (auto p : perms) {
		for (auto s : signs) {
			Matrix3x3 mat;
			mat.col[0] = raw_axes[p[0]] * s[0];
			mat.col[1] = raw_axes[p[1]] * s[1];
			mat.col[2] = raw_axes[p[2]] * s[2];

			if (mat.det() < 0.9f) continue;

			Matrix3x3 rot_mat;
			rot_mat.col[0] = normalize(mat.col[0]);

			// 剔除 col[1] 中平行于 col[0] 的分量，使其绝对垂直
			Vector3D proj_on_0 = rot_mat.col[0] * dot(mat.col[1], rot_mat.col[0]);
			rot_mat.col[1] = normalize(mat.col[1] - proj_on_0);

			// 用叉乘生成绝对完美垂直的第三轴
			rot_mat.col[2] = normalize(cross(rot_mat.col[0], rot_mat.col[1]));

			float score = 0;
			for (auto& info : face_infos) {
				// 使用洗干净的 rot_mat 进行局部化计算
				Vector3D n_local = rot_mat.transpose_mul(info.n_w);
				Vector3D T_u_local = rot_mat.transpose_mul(info.T_u);
				Vector3D T_v_local = rot_mat.transpose_mul(info.T_v);

				Vector3D exp_U, exp_V;
				get_expected_uv_dirs(n_local, exp_U, exp_V);

				score += std::abs(dot(T_u_local, exp_U));
				score += std::abs(dot(T_v_local, exp_V));
			}

			// 必须使用洗净后的矩阵去获取欧拉角
			Vector3D euler = matrix_to_euler_xyz(rot_mat);
			float euler_sum = std::abs(euler.x) + std::abs(euler.y) + std::abs(euler.z);

			if (score > best_score + 1e-4f) {
				best_score = score;
				best_rot_matrix = rot_mat; // 保存洗净的矩阵
				best_euler = euler;
				min_euler_sum = euler_sum;
			}
			else if (std::abs(score - best_score) <= 1e-4f && euler_sum < min_euler_sum) {
				best_rot_matrix = rot_mat; // 保存洗净的矩阵
				best_euler = euler;
				min_euler_sum = euler_sum;
			}
		}
	}

	Vector3D local_min = { 1e9f, 1e9f, 1e9f };
	Vector3D local_max = { -1e9f, -1e9f, -1e9f };

	for (const auto& pt : unique_pts) {
		Vector3D local_pt = best_rot_matrix.transpose_mul(pt);
		local_min.x = std::min(local_min.x, local_pt.x);
		local_min.y = std::min(local_min.y, local_pt.y);
		local_min.z = std::min(local_min.z, local_pt.z);

		local_max.x = std::max(local_max.x, local_pt.x);
		local_max.y = std::max(local_max.y, local_pt.y);
		local_max.z = std::max(local_max.z, local_pt.z);
	}

	Vector3D local_center = {
		(local_min.x + local_max.x) / 2.0f,
		(local_min.y + local_max.y) / 2.0f,
		(local_min.z + local_max.z) / 2.0f
	};

	Vector3D dump_center = best_rot_matrix.col[0] * local_center.x +
		best_rot_matrix.col[1] * local_center.y +
		best_rot_matrix.col[2] * local_center.z;

	Vector3D bb_pivot = { -dump_center.x, dump_center.y, dump_center.z };

	Vector3D size_signs = { 1.0f, 1.0f, 1.0f };
	for (int ax_idx = 0; ax_idx < 3; ++ax_idx) {
		Vector3D local_ax = best_rot_matrix.col[ax_idx];
		for (const auto& info : face_infos) {
			Vector3D n_local = best_rot_matrix.transpose_mul(info.n_w);
			float n_local_val = (ax_idx == 0) ? n_local.x : (ax_idx == 1 ? n_local.y : n_local.z);

			if (std::abs(n_local_val) > 0.5f) {
				Vector3D face_center = { 0, 0, 0 };
				for (int i = 0; i < 4; i++) {
					face_center = face_center + info.raw_f->vertices[i].vec * 16.0f;
				}
				face_center = face_center / 4.0f;
				Vector3D offset = face_center - dump_center;

				float dot_offset = dot(offset, local_ax);
				if (std::abs(dot_offset) > 1e-3f) {
					if (dot_offset * n_local_val < 0) {
						if (ax_idx == 0)      size_signs.x = -1.0f;
						else if (ax_idx == 1) size_signs.y = -1.0f;
						else                  size_signs.z = -1.0f;
					}
				}
				break;
			}
		}
	}

	Vector3D bb_rot = { -best_euler.x, -best_euler.y, best_euler.z };

	float sx_max = 0, sy_max = 0, sz_max = 0;
	for (auto pt : unique_pts) {
		Vector3D d = pt - dump_center;
		sx_max = std::max(sx_max, std::abs(dot(d, best_rot_matrix.col[0])));
		sy_max = std::max(sy_max, std::abs(dot(d, best_rot_matrix.col[1])));
		sz_max = std::max(sz_max, std::abs(dot(d, best_rot_matrix.col[2])));
	}

	Vector3D bb_size = {
		(sx_max * 2 - 2 * original_inflate) * size_signs.x,
		(sy_max * 2 - 2 * original_inflate) * size_signs.y,
		(sz_max * 2 - 2 * original_inflate) * size_signs.z
	};

	Vector3D bb_origin = bb_pivot - bb_size / 2.0f;

	BlockbenchCube result;
	result.origin = bb_origin;
	result.size = bb_size;
	result.inflate = original_inflate;
	result.pivot = bb_pivot;
	result.rotation = bb_rot;

	for (auto& info : face_infos) {
		Vector3D n_local = best_rot_matrix.transpose_mul(info.n_w);
		Vector3D T_u_local = best_rot_matrix.transpose_mul(info.T_u);
		Vector3D T_v_local = best_rot_matrix.transpose_mul(info.T_v);

		Vector3D raw_expected_U, raw_expected_V;
		get_expected_uv_dirs(n_local, raw_expected_U, raw_expected_V);
		Vector3D exp_U = raw_expected_U * size_signs;
		Vector3D exp_V = raw_expected_V * size_signs;

		float u_min = 1e9, v_min = 1e9, u_max = -1e9, v_max = -1e9;
		for (int i = 0; i < 4; i++) {
			float u = info.raw_f->vertices[i].u * texture_width;
			float v = info.raw_f->vertices[i].v * texture_height;
			u_min = std::min(u_min, u); u_max = std::max(u_max, u);
			v_min = std::min(v_min, v); v_max = std::max(v_max, v);
		}

		float u_start = u_min;
		float v_start = v_min;
		float u_sz = u_max - u_min;
		float v_sz = v_max - v_min;

		if (dot(T_u_local, exp_U) < -0.2f) { u_start = u_max; u_sz = -u_sz; }
		if (dot(T_v_local, exp_V) < -0.2f) { v_start = v_max; v_sz = -v_sz; }

		UVBox box = {
			clean_val(u_start), clean_val(v_start),
			clean_val(u_sz), clean_val(v_sz)
		};

		if (dot(n_local, { 1, 0, 0 }) > 0.9f) result.uv["east"] = box;
		else if (dot(n_local, { -1, 0, 0 }) > 0.9f) result.uv["west"] = box;
		else if (dot(n_local, { 0, 1, 0 }) > 0.9f) result.uv["up"] = box;
		else if (dot(n_local, { 0, -1, 0 }) > 0.9f) result.uv["down"] = box;
		else if (dot(n_local, { 0, 0, 1 }) > 0.9f) result.uv["south"] = box;
		else if (dot(n_local, { 0, 0, -1 }) > 0.9f) result.uv["north"] = box;
	}

	return result;
}

struct ParsedCube {
	std::vector<Face> faces;
};

struct ParsedBone {
	std::string parent;
	std::vector<ParsedCube> cubes;
	std::string name;
	Vector3D pivot;
	Vector3D rotation;
};

struct ParsedDescription {
	std::string identifier;
	float texture_width;
	float texture_height;
	float visible_bounds_width;
	float visible_bounds_height;
	std::vector<float> visible_bounds_offset;
};

struct ParsedModel {
	std::string sha256;
	ParsedDescription description;
	std::vector<ParsedBone> bones;
	float scaleX;
	float scaleY;
};


static float clean_number(float value) {
	if (std::abs(value) < 1e-9f) return 0.0f;
	float rounded_int = std::round(value);
	if (std::abs(value - rounded_int) < 1e-6f) return rounded_int;
	return std::round(value * 100000.0f) / 100000.0f; // retain 5 decimals
}

static std::vector<float> clean_vector(const Vector3D& vec) {
	return { clean_number(vec.x), clean_number(vec.y), clean_number(vec.z) };
}

//

std::vector<uint8_t> YSMParserV3::ParseModels(BufferReader& reader)
{
	reader.setContext("ParseModels");
	ParsedModel model;

	// Parse main.json
	if (m_format > 15) {
		model.sha256 = reader.readString();
	}
	uint32_t sizeOfBones = reader.readVarint();
	model.bones.reserve(sizeOfBones);

	for (uint32_t i = 0; i < sizeOfBones; i++)
	{
		ParsedBone bone;
		bone.parent = reader.readString();
		std::cout << "====\nparent: " << bone.parent << std::endl;
		//printf("read bone: 0x%08zX\n", reader.offset);
		uint32_t cubesSize = reader.readVarint();
		bone.cubes.reserve(cubesSize);

		for (uint32_t j = 0; j < cubesSize; j++)
		{
			ParsedCube cube;
			uint32_t uvSize = reader.readVarint();
			cube.faces.reserve(uvSize);
			for (uint32_t k = 0; k < uvSize; k++)
			{
				Face face{};
				face.normal = reader.readVector3D();
				for (int v = 0; v < 4; v++)
				{
					face.vertices[v].vec = reader.readVector3D();
					face.vertices[v].u = reader.readFloat();
					face.vertices[v].v = reader.readFloat();
				}
				cube.faces.emplace_back(face);
			}

			bone.cubes.emplace_back(std::move(cube));
			if (false) {
				for (size_t idx = 0; idx < cube.faces.size(); ++idx) {
					const Face& face = cube.faces[idx];
					printf("Face %zu: normal=(%f, %f, %f)\n", idx,
						face.normal.x, face.normal.y, face.normal.z);
					for (int v = 0; v < 4; ++v) {
						printf("  vertex[%d]: pos=(%f, %f, %f), uv=(%f, %f)\n", v,
							face.vertices[v].vec.x, face.vertices[v].vec.y, face.vertices[v].vec.z,
							face.vertices[v].u, face.vertices[v].v);
					}
				}
			}
			//printf("read bone: 0x%08zX\n", reader.offset);
			uint32_t unk_int1 = reader.readVarint(); uint32_t unk_int2 = reader.readVarint(); uint32_t unk_int3 = reader.readVarint();
			if (unk_int1 != 0 || unk_int2 != 0 || unk_int3 != 0) {
				printf("unk_int1=%u, unk_int2=%u, unk_int3=%u\n", unk_int1, unk_int2, unk_int3);
			}

		}

		bone.name = reader.readString();
		std::cout << "boneName: " << bone.name << std::endl;

		printf("bone offset++ : 0x%08zX\n", reader.offset);

		// TODO: 5 unknown bytes.
		if (reader.readVarint() != 0) throw ParserUnknownField();
		if (reader.readVarint() != 0) throw ParserUnknownField();
		if (reader.readVarint() != 0) throw ParserUnknownField();
		if (reader.readVarint() != 0) throw ParserUnknownField();
		// if (reader.readVarint() != 0) throw ParserUnknownField();
		reader.readVarint(); // 这个有可能是1

		// pivot: X轴反转，Y Z未反转
		bone.pivot.x = reader.readFloat();
		bone.pivot.y = reader.readFloat();
		bone.pivot.z = reader.readFloat();
		std::cout << "X:" << bone.pivot.x << " ,Y:" << bone.pivot.y << " ,Z:" << bone.pivot.z << std::endl;

		// rotation: X Y 反转，Z未反转 (原始写入可能单位是弧度)
		bone.rotation.x = reader.readFloat();
		bone.rotation.y = reader.readFloat();
		bone.rotation.z = reader.readFloat();
		std::cout << "rotationX:" << bone.rotation.x * (180.0 / M_PI)
			<< " ,rotationY:" << bone.rotation.y * (180.0 / M_PI)
			<< " ,rotationZ:" << bone.rotation.z * (180.0 / M_PI) << std::endl;

		model.bones.emplace_back(std::move(bone));

	}

	model.description.identifier = reader.readString();
	model.description.texture_height = reader.readFloat();
	model.description.texture_width = reader.readFloat();
	model.description.visible_bounds_height = reader.readFloat();
	model.description.visible_bounds_width = reader.readFloat();

	uint32_t visible_bounds_offset_size = reader.readVarint();
	model.description.visible_bounds_offset.reserve(visible_bounds_offset_size);
	for (uint32_t i = 0; i < visible_bounds_offset_size; i++)
	{
		model.description.visible_bounds_offset.push_back(reader.readFloat());
	}

	model.scaleX = reader.readFloat(); // 0.7
	model.scaleY = reader.readFloat(); // 0.7
	//if (m_format >= 4) // 这里应该是三个00
	//{
	//	model.unk3 = reader.readFloat(); // 0.0
	//}
	uint32_t hasInfoJson = reader.readVarint();
	if (hasInfoJson > 0) {
		ParseLegacyYSMInfo(reader);
	}

	reader.readVarint(); reader.readVarint(); reader.readVarint();


	using json = nlohmann::ordered_json;

	json out_geom = json::object();

	json description = json::object();
	description["identifier"] = model.description.identifier;
	description["texture_width"] = clean_number(model.description.texture_width);
	description["texture_height"] = clean_number(model.description.texture_height);
	description["visible_bounds_width"] = clean_number(model.description.visible_bounds_width);
	description["visible_bounds_height"] = clean_number(model.description.visible_bounds_height);

	json offset_arr = json::array();
	offset_arr.get_ref<json::array_t&>().reserve(model.description.visible_bounds_offset.size());
	for (float v : model.description.visible_bounds_offset) {
		offset_arr.push_back(clean_number(v));
	}
	description["visible_bounds_offset"] = offset_arr;

	out_geom["description"] = description;

	json bones_arr = json::array();
	bones_arr.get_ref<json::array_t&>().reserve(model.bones.size());
	for (const auto& parsedBone : model.bones) {
		json b_json = json::object();
		b_json["name"] = parsedBone.name;
		b_json["pivot"] = { clean_number(-parsedBone.pivot.x), clean_number(parsedBone.pivot.y), clean_number(parsedBone.pivot.z) };

		if (!parsedBone.parent.empty()) {
			b_json["parent"] = parsedBone.parent;
		}

		Vector3D rot = { (-parsedBone.rotation.x) * static_cast<float>(180.0 / M_PI), (-parsedBone.rotation.y) * static_cast<float>(180.0 / M_PI), (parsedBone.rotation.z) * static_cast<float>(180.0 / M_PI) };
		if (std::abs(rot.x) > 1e-6f || std::abs(rot.y) > 1e-6f || std::abs(rot.z) > 1e-6f) {
			b_json["rotation"] = clean_vector(rot);
		}

		json cubes_arr = json::array();
		cubes_arr.get_ref<json::array_t&>().reserve(parsedBone.cubes.size());
		for (const auto& parsedCube : parsedBone.cubes) {
			try {
				BlockbenchCube bbCube = restore_blockbench_cube(parsedCube.faces, 0.0f, static_cast<int>(model.description.texture_width), static_cast<int>(model.description.texture_height));
				json c_json = json::object();
				c_json["origin"] = clean_vector(bbCube.origin);
				c_json["size"] = clean_vector(bbCube.size);
				c_json["pivot"] = clean_vector(bbCube.pivot);
				c_json["rotation"] = clean_vector(bbCube.rotation);

				json uv_json = json::object();
				uv_json.get_ref<json::object_t&>().reserve(bbCube.uv.size());
				for (const auto& uv_pair : bbCube.uv) {
					uv_json[uv_pair.first] = {
						{"uv", { clean_number(uv_pair.second.u), clean_number(uv_pair.second.v) }},
						{"uv_size", { clean_number(uv_pair.second.u_size), clean_number(uv_pair.second.v_size) }}
					};
				}
				c_json["uv"] = std::move(uv_json);
				cubes_arr.push_back(std::move(c_json));
			}
			catch (...) {
				// TODO:
			}
		}

		if (!cubes_arr.empty()) {
			b_json["cubes"] = std::move(cubes_arr);
		}

		bones_arr.push_back(std::move(b_json));
	}
	out_geom["bones"] = std::move(bones_arr);

	json final_output = json::object();

	// Format Version Inference
	std::string format_version = "1.12.0";
	final_output["format_version"] = format_version;
	json geometries = json::array();
	geometries.push_back(std::move(out_geom));
	final_output["minecraft:geometry"] = std::move(geometries);

	std::string result;
	if (this->isFormatJson())
	{
		result = final_output.dump(4, ' ', false);
	}
	else
	{
		result = final_output.dump(-1);
	}
	return bytesFromString(result);
}


void YSMParserV3::ParseYSMJson(BufferReader& reader)
{
	reader.setContext("ParseYSMJson");
	using json = nlohmann::ordered_json;
	reader.setContext("ParseYSMJson.sha256");
	std::string sha256 = reader.readString();

	json root = json::object();

	m_metadata = json::object();
	json properties = json::object();

	root["spec"] = 2; // version?

	uint32_t isNewVerionYSM = reader.readVarint();
	printf("TEST1 0x%08zX\n", reader.offset);
	if (!isNewVerionYSM) {
		return;
	}

	if (m_format <= 15) {
		printf("NUM=%llu\n", reader.readVarint());
	}

	reader.setContext("ParseYSMJson.metadata.name");
	m_metadata["name"] = reader.readString();

	printf("TEST2 0x%08zX\n", reader.offset);
	reader.setContext("ParseYSMJson.metadata.tips");
	m_metadata["tips"] = reader.readString();
	printf("TEST3 0x%08zX\n", reader.offset);
	m_metadata["license"] = json::object();
	reader.setContext("ParseYSMJson.license.type");
	m_metadata["license"]["type"] = reader.readString();
	reader.setContext("ParseYSMJson.license.desc");
	m_metadata["license"]["desc"] = reader.readString();
	printf("TEST4 0x%08zX\n", reader.offset);
	uint32_t authorsCount = reader.readVarint();
	if (authorsCount > 0) {
		json authorsArr = json::array();
		for (uint32_t j = 0; j < authorsCount; j++) {
			json authorObj = json::object();
			reader.setContext("ParseYSMJson.authors.name");
			std::string authorName = reader.readString();
			authorObj["name"] = authorName;
			reader.setContext("ParseYSMJson.authors.role");
			authorObj["role"] = reader.readString();

			uint32_t authorContact = reader.readVarint();
			if (authorContact > 0) {
				json contactsObj = json::object();
				for (uint32_t k = 0; k < authorContact; k++) {
					reader.setContext("ParseYSMJson.authors.contact.name");
					std::string contactName = reader.readString();
					reader.setContext("ParseYSMJson.authors.contact.site");
					std::string contactSite = reader.readString();
					contactsObj[contactName] = contactSite;
				}
				authorObj["contact"] = contactsObj;
			}

			reader.setContext("ParseYSMJson.authors.comment");
			authorObj["comment"] = reader.readString();
			authorObj["avatar"] = "avatar/" + sanitizeWindowsFilename(authorName + ".png");
			authorsArr.push_back(authorObj);
		}
		m_metadata["authors"] = authorsArr;
	}

	// 解析 links
	uint32_t linksCount = reader.readVarint();
	if (linksCount > 0) {
		json linksObj = json::object();
		for (uint32_t j = 0; j < linksCount; j++) {
			reader.setContext("ParseYSMJson.links.name");
			std::string linkName = reader.readString();
			reader.setContext("ParseYSMJson.links.site");
			std::string linkSite = reader.readString();
			linksObj[linkName] = linkSite;
		}
		m_metadata["link"] = linksObj;
	}

	// 解析 properties 缩放参数
	properties["width_scale"] = reader.readFloat();
	properties["height_scale"] = reader.readFloat();

	// 解析 properties.extra_animation
	uint32_t extra_animations = reader.readVarint();
	if (extra_animations > 0) {
		json extraAnimObj = json::object();
		for (uint32_t j = 0; j < extra_animations; j++) {
			reader.setContext("ParseYSMJson.extra_animation.name");
			std::string extraName = reader.readString();
			reader.setContext("ParseYSMJson.extra_animation.value");
			std::string extraValue = reader.readString();
			extraAnimObj[extraName] = extraValue;
			printf("pushing %s -> %s\n", extraName.c_str(), extraValue.c_str());
		}
		properties["extra_animation"] = extraAnimObj;
	}

	if (m_format > 9) {
		printf("extra_animation_buttons 0x%08zX\n", reader.offset);

		// 解析 properties.extra_animation_buttons
		uint32_t extra_animation_buttons = reader.readVarint();
		if (extra_animation_buttons > 0) {
			json buttonsArr = json::array();
			for (uint32_t j = 0; j < extra_animation_buttons; j++) {
				json buttonObj = json::object();
				reader.setContext("ParseYSMJson.extra_animation_buttons.id");
				std::string id = reader.readString();
				std::cout << "[properties.extra_animation_buttons.id]" << id << std::endl;
				buttonObj["id"] = id;
				reader.setContext("ParseYSMJson.extra_animation_buttons.name");
				buttonObj["name"] = reader.readString();

				if (reader.readVarint() != 0)  throw ParserUnknownField();

				uint32_t config_forms = reader.readVarint();
				if (config_forms > 0) {
					json configArr = json::array();
					for (uint32_t k = 0; k < config_forms; k++) {
						json formObj = json::object();
						reader.setContext("ParseYSMJson.config_forms.type");
						std::string type = reader.readString();
						formObj["type"] = type;
						reader.setContext("ParseYSMJson.config_forms.title");
						formObj["title"] = reader.readString();
						reader.setContext("ParseYSMJson.config_forms.description");
						formObj["description"] = reader.readString();
						reader.setContext("ParseYSMJson.config_forms.value");
						formObj["value"] = reader.readString();

						float step = reader.readFloat();
						float _min = reader.readFloat();
						float _max = reader.readFloat();

						// 只有 type 为 range 时，才把 step, min, max 写入 JSON
						if (type == "range") {
							formObj["step"] = step;
							formObj["min"] = _min;
							formObj["max"] = _max;
						}

						uint32_t labelsSize = reader.readVarint();
						if (labelsSize > 0) {
							json labelsObj = json::object();
							for (uint32_t l = 0; l < labelsSize; l++) {
								reader.setContext("ParseYSMJson.config_forms.labels.name");
								std::string labelName = reader.readString();
								reader.setContext("ParseYSMJson.config_forms.labels.molang");
								std::string labelMolang = reader.readString();
								labelsObj[labelName] = labelMolang;
							}
							formObj["labels"] = labelsObj;
						}
						configArr.push_back(formObj);
					}
					buttonObj["config_forms"] = configArr;
				}
				buttonsArr.push_back(buttonObj);
			}
			properties["extra_animation_buttons"] = buttonsArr;
		}

		uint32_t cnt = reader.readVarint();
		json extraAnimationClassify = json::array();
		for (uint32_t i = 0; i < cnt; i++)
		{
			json sigObj = json::object();
			reader.setContext("ParseYSMJson.extra_animation_classify.id");
			std::string id = reader.readString();
			sigObj["id"] = id;
			uint32_t extras = reader.readVarint();
			json labelsObj = json::object();
			for (uint32_t j = 0; j < extras; j++)
			{
				reader.setContext("ParseYSMJson.extra_animation_classify.key");
				std::string ext_key = reader.readString();
				reader.setContext("ParseYSMJson.extra_animation_classify.value");
				std::string ext_v = reader.readString();
				labelsObj[ext_key] = ext_v;
			}
			sigObj["extra_animation"] = labelsObj;
			extraAnimationClassify.push_back(sigObj);
		}
		properties["extra_animation_classify"] = extraAnimationClassify;
	}


	reader.setContext("ParseYSMJson.default_texture");
	if (auto val = reader.readString(); !val.empty()) {
		properties["default_texture"] = std::move(val);
	}

	reader.setContext("ParseYSMJson.preview_animation");
	if (auto val = reader.readString(); !val.empty()) {
		properties["preview_animation"] = std::move(val);
	}

	// bool
	properties["free"] = (bool)reader.readVarint();

	if (m_format > 4) {
		properties["render_layers_first"] = (bool)reader.readVarint();
	}

	if (m_format >= 15) {
		properties["all_cutout"] = (bool)reader.readVarint();
		properties["disable_preview_rotation"] = (bool)reader.readVarint();
	}


	// https://github.com/YesSteveModel/YSM-Wiki-Issues/blob/cb7015badedcab90683d6a266150595155ae8fad/docs/notes/wiki/%E6%A8%A1%E5%9E%8B%E5%8C%85%E5%88%B6%E4%BD%9C/%E9%A1%B9%E7%9B%AE%E7%BB%93%E6%9E%84.md?plain=1#L354
	// https://github.com/YesSteveModel/YSM-Wiki-Issues/blob/cb7015badedcab90683d6a266150595155ae8fad/docs/notes/wiki/%E6%A8%A1%E5%9E%8B%E5%8C%85%E5%88%B6%E4%BD%9C/%E5%89%8D%E6%99%AF%E5%9B%BE%E4%B8%8E%E8%83%8C%E6%99%AF%E5%9B%BE.md?plain=1#L17
	std::string gui_foreground;
	std::string gui_background;

	// 15 2.3.1
	// 26 2.5.0
	if (m_format > 15) {
		printf("gui_no_lighting 0x%08zX\n", reader.offset);
		properties["gui_no_lighting"] = (bool)reader.readVarint();

		if (m_format >= 32) {
			properties["merge_multiline_expr"] = (bool)reader.readVarint();
		}

		gui_foreground = reader.readString();
		if (!gui_foreground.empty()) {
			properties["gui_foreground"] = gui_foreground;
		}

		gui_background = reader.readString();
		if (!gui_background.empty()) {
			properties["gui_background"] = gui_background;
		}
		printf("gui_background end 0x%08zX\n", reader.offset);
		// 解析 Avatars

		uint32_t avatars = reader.readVarint();
		if (avatars > 0) {
			json avatarsMeta = json::array();
			for (uint32_t j = 0; j < avatars; j++) {
				json avatarInfo = json::object();
				std::string avatarName = reader.readString();
				avatarInfo["name"] = avatarName;

				std::vector<uint8_t> data = reader.readByteSequence();

				avatarInfo["width"] = reader.readVarint();
				avatarInfo["height"] = reader.readVarint();

				uint32_t image_format = reader.readVarint();
				uint32_t unk_flag = reader.readVarint();
				Images::validateImageMetadata(avatarName, reader.offset, image_format, unk_flag);
				appendFile(m_avatarFiles, std::move(avatarName), std::move(data));

				avatarsMeta.push_back(avatarInfo);
			}
		}
	}

	// 拼装最终结构
	root["metadata"] = m_metadata;
	root["properties"] = properties;

	root["files"] = buildFilesFromParsedData();

	clean_json_floats(root);

	std::string result;
	if (this->isFormatJson())
	{
		result = root.dump(4, ' ', false);
	}
	else
	{
		result = root.dump(-1);
	}

	m_ysmJsonFile = bytesFromString(result);
	if (m_format <= 15) return;


	if (!gui_foreground.empty() || !gui_background.empty()) { // TODO:为什么这里需要特判，按理来说没数据读取应该是0?
		uint32_t bgCount = reader.readVarint();
		for (uint32_t i = 0; i < bgCount; i++) {
			std::string name = reader.readString();
			printf("Reading background Image: %s\n", name.c_str());

			std::vector<uint8_t> fileData = reader.readByteSequence();

			std::string relativePath;
			if (name == "gui_foreground" && !gui_foreground.empty()) {
				relativePath = gui_foreground;
			}
			else if (name == "gui_background" && !gui_background.empty()) {
				relativePath = gui_background;
			}
			else {
				relativePath = "background/" + sanitizeWindowsFilename(name + ".png");
			}

			printf("Finish background at: 0x%08zX\n", reader.offset);
			uint32_t width = reader.readVarint();
			uint32_t height = reader.readVarint();
			(void)width;
			(void)height;
			uint32_t image_format = reader.readVarint();
			uint32_t unk_flag = reader.readVarint();
			Images::validateImageMetadata(name, reader.offset, image_format, unk_flag);
			appendFile(m_backgroundFiles, std::move(relativePath), std::move(fileData));
		}
	}
}

nlohmann::ordered_json YSMParserV3::buildFilesFromParsedData() {
	using json = nlohmann::ordered_json;
	json files = json::object();

	std::set<std::string> subEntityModels;
	for (const auto& [name, data] : m_modelFiles) {
		if (name != "main" && name != "arm") {
			subEntityModels.insert(name);
		}
	}

	static const std::unordered_map<std::string, std::string> knownSubModels = {
		{"arrow", "projectiles"},
		{"trident", "projectiles"},
		{"horse", "vehicles"},
		{"minecart", "vehicles"},
		{"boat", "vehicles"},
	};

	auto getCategory = [&](const std::string& modelName) -> std::string {
		auto it = m_subEntityCategories.find(modelName);
		if (it != m_subEntityCategories.end()) {
			if (it->second == "vehicle") return "vehicles";
			if (it->second == "SubEntity") {
				auto k = knownSubModels.find(modelName);
				if (k != knownSubModels.end()) return k->second;
			}
			return it->second;
		}
		auto k = knownSubModels.find(modelName);
		if (k != knownSubModels.end()) return k->second;
		return "sub_entities";
		};

	auto normalizeAnimKey = [](const std::string& name) -> std::string {
		if (name == "fp.arm" || name == "fp_arm") return "fp_arm";
		if (name == "iss" || name == "irons_spell_books") return "irons_spell_books";
		return name;
		};

	auto animPathComponent = [](const std::string& key) -> std::string {
		if (key == "fp_arm") return "fp.arm";
		if (key == "irons_spell_books") return "iss";
		return key;
		};

	// --- Player section ---
	json player = json::object();

	json playerModels = json::object();
	for (const auto& [name, data] : m_modelFiles) {
		if (name == "main") playerModels["main"] = "models/main.json";
		else if (name == "arm") playerModels["arm"] = "models/arm.json";
	}
	if (!playerModels.empty()) player["model"] = playerModels;

	json playerAnims = json::object();
	for (const auto& [name, data] : m_animationFiles) {
		if (name.find('/') != std::string::npos) continue;
		if (subEntityModels.count(name)) continue;
		std::string key = normalizeAnimKey(name);
		playerAnims[key] = "animations/" + sanitizeWindowsFilename(animPathComponent(key) + ".animation.json");
	}
	if (!playerAnims.empty()) player["animation"] = playerAnims;

	json ac1 = json::array();
	for (const auto& [name, data] : m_animControllerFiles) {
		if (name.find('/') != std::string::npos) continue;
		if (subEntityModels.count(name)) continue;
		std::string key = normalizeAnimKey(name);
		ac1.push_back("controller/" + sanitizeWindowsFilename(name + ".json"));
	}
	player["animation_controllers"] = ac1;

	json playerTex = json::array();
	for (const auto& [name, data] : m_textureFiles) {
		bool isSub = false;
		for (const auto& sn : subEntityModels) {
			if (name == sn || name.starts_with(sn + "_")) { isSub = true; break; }
		}
		if (isSub) continue;
		json texObj = json::object();
		texObj["uv"] = "textures/" + sanitizeWindowsFilename(name + ".png");

		std::string expectedNormal = name + "_normal";
		std::string expectedSpecular = name + "_specular";

		for (const auto& spImg : m_specialImageFiles) {
			if (spImg.first == expectedNormal) {
				texObj["normal"] = "textures/" + sanitizeWindowsFilename(expectedNormal + ".png");
			}
			else if (spImg.first == expectedSpecular) {
				texObj["specular"] = "textures/" + sanitizeWindowsFilename(expectedSpecular + ".png");
			}
		}

		playerTex.push_back(texObj);
	}
	if (!playerTex.empty()) player["texture"] = playerTex;

	files["player"] = player;

	// --- Sub-entity sections ---
	std::map<std::string, std::vector<std::string>> catModels;
	for (const auto& mn : subEntityModels) {
		catModels[getCategory(mn)].push_back(mn);
	}

	for (const auto& [cat, models] : catModels) {
		json section = json::object();
		for (const auto& mn : models) {
			json entry = json::object();

			entry["model"] = "models/" + sanitizeWindowsFilename(mn + ".json");

			for (const auto& [animName, animData] : m_animationFiles) {
				bool match = false;
				std::string animPath;
				if (animName.find('/') != std::string::npos) {
					auto sp = animName.find('/');
					if (animName.substr(sp + 1) == mn) {
						match = true;
						std::string catPrefix = animName.substr(0, sp + 1);
						std::string baseName = animName.substr(sp + 1);
						animPath = "animations/" + catPrefix + sanitizeWindowsFilename(baseName + ".animation.json");
					}
				}
				else if (animName == mn) {
					match = true;
					if (cat == "vehicles")
						animPath = "animations/vehicle/" + sanitizeWindowsFilename(mn + ".animation.json");
					else
						animPath = "animations/" + sanitizeWindowsFilename(mn + ".animation.json");
				}
				if (match) { entry["animation"] = animPath; break; }
			}

			for (const auto& [animName, animData] : m_animControllerFiles) {
				bool match = false;
				std::string animPath;
				if (animName.find('/') != std::string::npos) {
					auto sp = animName.find('/');
					if (animName.substr(sp + 1) == mn) {
						match = true;
						std::string catPrefix = animName.substr(0, sp + 1);
						std::string baseName = animName.substr(sp + 1);
						animPath = "controller/" + catPrefix + sanitizeWindowsFilename(baseName + ".json");
					}
				}
				else if (animName == mn) {
					match = true;
					if (cat == "vehicles")
						animPath = "controller/vehicle/" + sanitizeWindowsFilename(mn + ".json");
					else
						animPath = "controller/" + sanitizeWindowsFilename(mn + ".json");
				}
				if (match) { entry["controller"] = animPath; break; }
			}

			for (const auto& [texName, texData] : m_textureFiles) {
				if (texName == mn || texName.starts_with(mn + "_")) {
					entry["texture"] = "textures/" + sanitizeWindowsFilename(mn + ".png");
					break;
				}
			}

			section[mn] = entry;
		}
		files[cat] = section;
	}

	return files;
}

void YSMParserV3::ParseLegacyYSMInfo(BufferReader& reader)
{
	using json = nlohmann::ordered_json;

	std::string info_name = reader.readString();
	std::string info_tips = reader.readString();
	uint32_t extraAnims = reader.readVarint();
	json jinfo_extra_animation_names = json::array();
	for (uint32_t i = 0; i < extraAnims; i++)
	{
		jinfo_extra_animation_names.push_back(reader.readString());
	}
	uint32_t authors = reader.readVarint();
	json jAuthors = json::array();
	for (uint32_t i = 0; i < authors; i++)
	{
		jAuthors.push_back(reader.readString());
	}
	std::string info_license = reader.readString();
	bool info_free = (bool)reader.readVarint();


	json j = json::object();
	j["name"] = info_name;
	j["tips"] = info_tips;
	j["authors"] = jAuthors;
	j["license"] = info_license;
	j["extra_animation_names"] = jinfo_extra_animation_names;
	j["free"] = info_free;


	std::string result;
	if (this->isFormatJson())
	{
		result = j.dump(4, ' ', false);
	}
	else
	{
		result = j.dump(-1);
	}

	m_infoJsonFile.assign(result.begin(), result.end());

	std::cout << "[DEBUG - info.json]\n" << result << std::endl;
}

std::vector<uint8_t> YSMParserV3::ParseAnimations(BufferReader& reader)
{
	reader.setContext("ParseAnimations");
	using json = nlohmann::ordered_json;
	auto log_animation_header = [&](const std::string& animName, float animLen, LoopMode loop, uint32_t boneCount) {
		if (!isVerbose()) {
			return;
		}

		std::cout << "\n[ANIMATION] " << animName << '\n';
		std::cout << "  |- length : " << animLen << '\n';
		std::cout << "  |- loop   : ";
		switch (loop) {
		case LoopMode::ONCE:
			std::cout << "once";
			break;
		case LoopMode::LOOP:
			std::cout << "loop";
			break;
		case LoopMode::HOLD_ON_LAST_FRAME:
			std::cout << "hold_on_last_frame";
			break;
		default:
			std::cout << "unknown(" << static_cast<int>(loop) << ")";
			break;
		}
		std::cout << '\n';
		std::cout << "  `- bones  : " << boneCount << '\n';
		};

	auto log_bone_summary = [&](const std::string& boneName,
		const std::optional<BonesKeyFrame>& rotation,
		const std::optional<BonesKeyFrame>& position,
		const std::optional<BonesKeyFrame>& scale) {
			if (!isVerbose()) {
				return;
			}

			auto frame_count = [](const std::optional<BonesKeyFrame>& channel) -> std::size_t {
				return channel.has_value() ? channel->bone.size() : 0;
				};

			auto log_channel = [&](const char* label, const std::optional<BonesKeyFrame>& channel) {
				std::cout << "     |- " << std::left << std::setw(8) << label
					<< ": " << frame_count(channel) << " keyframe(s)";

				if (channel.has_value() && !channel->bone.empty()) {
					const auto& first = channel->bone.front();
					const auto& last = channel->bone.back();
					std::cout << " [first=" << first.first << ", last=" << last.first
						<< ", lerp=" << lerp_mode_to_string(first.second.lerp_mode) << "]";
				}
				std::cout << '\n';
				};

			std::cout << "  |- Bone: " << boneName << '\n';
			log_channel("rotation", rotation);
			log_channel("position", position);
			log_channel("scale", scale);
		};

	auto log_animation_footer = [&](const std::string& animName,
		const std::optional<TimeLine>& timeline,
		const std::optional<Effects>& effects) {
			if (!isVerbose()) {
				return;
			}

			std::cout << "  |- timeline      : "
				<< (timeline.has_value() ? timeline->times.size() : 0) << " event group(s)\n";
			std::cout << "  `- sound effects : "
				<< (effects.has_value() ? effects->effects.size() : 0) << " item(s)\n";
			std::cout << "[ANIMATION DONE] " << animName << "\n";
		};

	// 创建根 JSON 对象
	json root = json::object();
	root["format_version"] = "1.8.0";
	json animationsDesc = json::object();

	std::string hash;
	if (m_format > 15) {
		hash = reader.readString();
	}
	uint32_t animCount = reader.readVarint();
	animationsDesc.get_ref<json::object_t&>().reserve(animCount);
	for (uint32_t anim = 0; anim < animCount; ++anim) {
		// 读取动画名称 (使用 Varint 长度)
		std::string animName = reader.readString();
		json animObj = json::object();

		// 读取动画长度 "animation_length"
		float animLen = reader.readFloat() / 20;
		if (animLen > 0.0f && !std::isinf(animLen)) {
			animObj["animation_length"] = animLen;
		}


		// 是否循环 "loop"
		LoopMode loop = static_cast<LoopMode>(reader.readVarint());
		if (loop == LoopMode::LOOP) {
			animObj["loop"] = true;
		}
		else if (loop == LoopMode::HOLD_ON_LAST_FRAME) {
			animObj["loop"] = "hold_on_last_frame";
		}

		if (m_format > 9) {
			if(reader.readVarint() != 0){
				throw ParserUnknownField();
			}

			if (reader.readVarint() != 0) {
				throw ParserUnknownField();
			}


			// blend_weight
			uint32_t blend_weightMolangs = reader.readVarint();
			for (uint32_t molangI = 0; molangI < blend_weightMolangs; molangI++) {
				printf("Finish blend_weightMolangs at: 0x%08zX\n", reader.offset);
				uint8_t type = reader.readByte();
				if (type == 0x01) {
					animObj["blend_weight"] = reader.readFloat();
				}
				else if (type == 0x02) {
					animObj["blend_weight"] = reader.readString();
				}
			}

			uint32_t unkint4 = reader.readVarint(); if (unkint4 != 0) { printf("unkint: 0x%08zX\n", reader.offset); throw ParserUnknownField(); }
		}

		// 读取 Bone 数量 (Varint) "bones"
		uint32_t boneCount = reader.readVarint();
		log_animation_header(animName, animLen, loop, boneCount);
		json bonesObj = json::object();
		bonesObj.get_ref<json::object_t&>().reserve(boneCount);

		auto molangToJson = [](const MolangPair& mp) -> json {
			// 检查三个元素是否类型相同且值一致
			if (mp.m[0].index() == mp.m[1].index() && mp.m[1].index() == mp.m[2].index()) {
				if (std::holds_alternative<float>(mp.m[0])) {
					float f0 = std::get<float>(mp.m[0]);
					float f1 = std::get<float>(mp.m[1]);
					float f2 = std::get<float>(mp.m[2]);
					if (std::abs(f0 - f1) < 1e-6f && std::abs(f1 - f2) < 1e-6f) {
						return (std::abs(f0) < 1e-6f) ? 0.0f : f0; // 统一 0.0
					}
				}
				else if (std::holds_alternative<std::string>(mp.m[0])) {
					const auto& s0 = std::get<std::string>(mp.m[0]);
					if (s0 == std::get<std::string>(mp.m[1]) && s0 == std::get<std::string>(mp.m[2])) {
						return s0;
					}
				}
			}

			// 无法化简，则构建数组
			json arr = json::array();
			for (int i = 0; i < 3; i++) {
				std::visit([&arr](auto&& arg) {
					using T = std::decay_t<decltype(arg)>;
					if constexpr (std::is_same_v<T, float>) {
						arr.push_back((std::abs(arg) < 1e-6f) ? 0.0f : arg);
					}
					else {
						arr.push_back(arg);
					}
					}, mp.m[i]);
			}
			return arr;
			};

		// 2. 优化后的 channelToJson 解析
		auto channelToJson = [&molangToJson](const std::optional<BonesKeyFrame>& channel) -> json {
			if (!channel.has_value() || channel->bone.empty()) return nullptr;

			// 如果只有一个关键帧，直接返回
			if (channel->bone.size() == 1 && channel->bone[0].first == 0.0f &&
				!channel->bone[0].second.pre.has_value() &&
				channel->bone[0].second.lerp_mode == LerpMode::LINEAR) {
				return molangToJson(channel->bone[0].second.post);
			}

			json keyframes = json::object();

			// magic things
			using base_vector_t = std::vector<std::pair<const std::string, json>>;
			auto& vec = static_cast<base_vector_t&>(keyframes.get_ref<json::object_t&>());

			vec.reserve(channel->bone.size());
			std::string t_str;
			t_str.reserve(32);
			for (const auto& kf_pair : channel->bone) {
				float time = kf_pair.first;
				const Keyframe& kf = kf_pair.second;
				if (std::abs(time) < 1e-6f) {
					t_str = "0.0";
				}
				else {
					t_str = formatKeyframeTime(time);
				}


				json val;
				if (!kf.pre.has_value() && kf.lerp_mode == LerpMode::LINEAR) {
					val = molangToJson(kf.post);
				}
				else {
					json kfData = json::object();
					if (kf.pre.has_value()) {
						kfData["post"] = molangToJson(kf.post);
						kfData["pre"] = molangToJson(kf.pre.value());
					}
					else {
						kfData["post"] = molangToJson(kf.post);
					}

					if (kf.lerp_mode == LerpMode::STEP) {
						kfData["lerp_mode"] = "step";
					}
					else if (kf.lerp_mode == LerpMode::CATMULLROM) {
						kfData["lerp_mode"] = "catmullrom";
					}
					val = std::move(kfData);
				}

				if (!vec.empty() && vec.back().first == t_str) {
					vec.back().second = std::move(val); // 应该不会触发
				}
				else {
					vec.emplace_back(std::move(t_str), std::move(val));
				}
			}
			return keyframes;
			};

		for (uint32_t i = 0; i < boneCount; ++i) {
			const auto boneNameValue = reader.readString();
			json boneData = json::object();

			auto rotation = parseChannel(reader);
			auto position = parseChannel(reader);
			auto scale = parseChannel(reader);
			log_bone_summary(boneNameValue, rotation, position, scale);

			json rotJson = channelToJson(rotation);
			if (!rotJson.is_null()) boneData["rotation"] = std::move(rotJson);

			json posJson = channelToJson(position);
			if (!posJson.is_null()) boneData["position"] = std::move(posJson);

			json scaleJson = channelToJson(scale);
			if (!scaleJson.is_null()) boneData["scale"] = std::move(scaleJson);

			if (!boneData.empty()) {
				bonesObj[boneNameValue] = std::move(boneData);
			}
		}

		if (!bonesObj.empty()) {
			animObj["bones"] = std::move(bonesObj);
		}

		auto tl = parseTimeLine(reader);
		if (tl.has_value() && !tl->times.empty()) {
			json tlObj = json::object();
			tlObj.get_ref<json::object_t&>().reserve(tl->times.size());
			for (const auto& item : tl->times) {
				std::ostringstream time_ss;
				time_ss << item.first;
				json eventArr = json::array();
				eventArr.get_ref<json::array_t&>().reserve(item.second.size());
				for (const auto& str : item.second) {
					eventArr.push_back(str);
				}
				tlObj[time_ss.str()] = std::move(eventArr);
			}
			animObj["timeline"] = std::move(tlObj);
		}

		if (m_format > 9) {
			auto sound_effects = parseEffect(reader);

			if (sound_effects.has_value() && !sound_effects->effects.empty()) {
				json efObj = json::object();
				efObj.get_ref<json::object_t&>().reserve(sound_effects->effects.size());
				for (const auto& item : sound_effects->effects) {
					std::ostringstream time_ss;
					if (std::abs(item.first) < 1e-6f) time_ss << "0.0";
					else time_ss << item.first;

					json effItem = json::object();
					effItem["effect"] = item.second;

					efObj[time_ss.str()] = std::move(effItem);
				}
				animObj["sound_effects"] = std::move(efObj);
			}
			log_animation_footer(animName, tl, sound_effects);
		}
		else {
			log_animation_footer(animName, tl, std::nullopt);
		}

		animationsDesc[animName] = std::move(animObj);

	}


	if (!animationsDesc.empty()) {
		root["animations"] = std::move(animationsDesc);
	}

	std::string result;
	if (this->isFormatJson())
	{
		result = root.dump(4, ' ', false);
	}
	else
	{
		result = root.dump(-1);
	}

	return bytesFromString(result);
}

std::vector<uint8_t> YSMParserV3::ParseSpecialImage(BufferReader& reader)
{
	reader.setContext("ParseSpecialImage");
	std::string hash = reader.readString();
	return reader.readByteSequence();
}

void YSMParserV3::ParseSoundFiles(BufferReader& reader)
{
	reader.setContext("ParseSoundFiles");
	uint32_t cnt = reader.readVarint();
	m_soundFiles.reserve(m_soundFiles.size() + cnt);
	for (uint32_t i = 0; i < cnt; i++)
	{
		std::string name = reader.readString();
		if (m_format > 15) {
			std::string hash = reader.readString();
		}
		std::vector<uint8_t> fileData = reader.readByteSequence();
		appendFile(m_soundFiles, std::move(name), std::move(fileData));
	}
}

void YSMParserV3::ParseFunctionFiles(BufferReader& reader)
{
	reader.setContext("ParseFunctionFiles");
	uint32_t cnt = reader.readVarint();
	m_functionFiles.reserve(m_functionFiles.size() + cnt);
	for (uint32_t i = 0; i < cnt; i++)
	{
		std::string name = reader.readString();
		std::string hash = reader.readString();
		std::vector<uint8_t> fileData = reader.readByteSequence();
		appendFile(m_functionFiles, std::move(name), std::move(fileData));
	}
}

void YSMParserV3::ParseLanguageFiles(BufferReader& reader)
{
	reader.setContext("ParseLanguageFiles");
	using json = nlohmann::ordered_json;
	uint32_t cnt = reader.readVarint();
	m_languageFiles.reserve(m_languageFiles.size() + cnt);
	// 创建根 JSON 数组，用于存放多个语言文件对象
	for (uint32_t i = 0; i < cnt; i++) {
		std::string name = reader.readString();
		std::string hash = reader.readString();

		// 创建一个对象来存放键值对
		json nodesData = json::object();

		uint32_t nodes = reader.readVarint();
		nodesData.get_ref<json::object_t&>().reserve(nodes);
		for (uint32_t j = 0; j < nodes; j++) {
			std::string nodeName = reader.readString();
			std::string nodeValue = reader.readString();

			// 将节点存入 nodes 对象
			nodesData[std::move(nodeName)] = std::move(nodeValue);
		}
		std::string result;
		if (this->isFormatJson())
		{
			result = nodesData.dump(4, ' ', false);
		}
		else
		{
			result = nodesData.dump(-1);
		}
		appendFile(m_languageFiles, std::move(name), bytesFromString(result));
	}
}

std::vector<uint8_t> YSMParserV3::ParseAnimationControllers(BufferReader& reader)
{
	reader.setContext("ParseAnimationControllers");
	auto log_controller_header = [&](const std::string& controllerName, const std::string& animName, uint32_t statesCount, const std::string& initialState) {
		if (!isVerbose()) {
			return;
		}

		std::cout << "\n[CONTROLLER] "
			<< (animName.empty() ? "<unnamed>" : animName);
		if (!controllerName.empty()) {
			std::cout << "  <" << controllerName << '>';
		}
		std::cout << '\n';
		std::cout << "  |- states        : " << statesCount << '\n';
		std::cout << "  `- initial state : " << (initialState.empty() ? "<none>" : initialState) << '\n';
		};

	auto log_state_block = [&](const std::string& stateName,
		const nlohmann::json* animations,
		const nlohmann::json* transitions,
		const nlohmann::json* onEntry,
		const nlohmann::json* onExit,
		const std::optional<float>& blendValue,
		const nlohmann::json* blendTransitions,
		bool shortestPath) {
			if (!isVerbose()) {
				return;
			}

			std::cout << "  |- State: " << stateName << '\n';
			if (animations != nullptr && !animations->empty()) {
				std::cout << Images::indent_multiline("     animations:\n" + animations->dump(2, ' ', false) + '\n', "     ");
			}
			if (transitions != nullptr && !transitions->empty()) {
				std::cout << Images::indent_multiline("     transitions:\n" + transitions->dump(2, ' ', false) + '\n', "     ");
			}
			if (onEntry != nullptr && !onEntry->empty()) {
				std::cout << Images::indent_multiline("     on_entry:\n" + onEntry->dump(2, ' ', false) + '\n', "     ");
			}
			if (onExit != nullptr && !onExit->empty()) {
				std::cout << Images::indent_multiline("     on_exit:\n" + onExit->dump(2, ' ', false) + '\n', "     ");
			}
			if (blendValue.has_value()) {
				std::cout << "     blend_transition: " << blendValue.value() << '\n';
			}
			if (blendTransitions != nullptr && !blendTransitions->empty()) {
				std::cout << Images::indent_multiline("     blend_transitions:\n" + blendTransitions->dump(2, ' ', false) + '\n', "     ");
			}
			if (shortestPath) {
				std::cout << "     blend_via_shortest_path: true\n";
			}
		};


	using namespace nlohmann;
	const bool verbose = isVerbose();
	// 创建根 JSON 对象
	json root = json::object();
	root["format_version"] = "1.19.0";



	json controllers = json::object();

	// 读取控制器数量
	uint32_t animCount = reader.readVarint();
	for (uint32_t anim = 0; anim < animCount; ++anim) {
		std::string animName = reader.readString();
		json controllerData = json::object();

		// 读取 initial_state (可能为空)
		std::string initialState = reader.readString();
		if (!initialState.empty()) {
			controllerData["initial_state"] = initialState;
		}

		json statesData = json::object();
		uint32_t statesCount = reader.readVarint();
		// log_controller_header(controllerName, animName, statesCount, initialState);
		//printf("statesCount=%i, 0x%08zX\n", statesCount, reader.offset);

		for (uint32_t i = 0; i < statesCount; i++) {
			std::string stateName = reader.readString(); // 例如 "default", "挥剑1"
			//printf("stateName=%s, 0x%08zX\n", stateName.c_str(), reader.offset);
			json stateObj = json::object();
			json debugAnimations;
			json debugTransitions;
			json debugOnEntry;
			json debugOnExit;
			json debugBlendTransitions;
			std::optional<float> debugBlendValue;
			bool debugShortestPath = false;

			// 1. 解析 Animations
			uint32_t animationsSize = reader.readVarint();
			//printf("animationsSize=%i, 0x%08zX\n", animationsSize, reader.offset);
			if (animationsSize > 0) {
				json animArray = json::array();
				animArray.get_ref<json::array_t&>().reserve(animationsSize);
				for (uint32_t j = 0; j < animationsSize; j++) {
					std::string ak = reader.readString();
					std::string av = reader.readString();
					//printf("ak=%s, av=%s, 0x%08zX\n", ak.c_str(), av.c_str(), reader.offset);
					if (av.empty()) {
						animArray.push_back(std::move(ak));
					}
					else {
						json animItem = json::object();
						animItem[std::move(ak)] = std::move(av);
						animArray.push_back(std::move(animItem));
					}
				}
				if (verbose) {
					debugAnimations = animArray;
				}
				stateObj["animations"] = std::move(animArray);
			}

			//  Transitions
			uint32_t transitionsSize = reader.readVarint();
			if (transitionsSize > 0) {
				json transArray = json::array();
				transArray.get_ref<json::array_t&>().reserve(transitionsSize);
				for (uint32_t j = 0; j < transitionsSize; j++) {
					std::string tk = reader.readString();
					std::string tv = reader.readString();

					json transItem = json::object();
					transItem[std::move(tk)] = std::move(tv);
					transArray.push_back(std::move(transItem));
				}
				if (verbose) {
					debugTransitions = transArray;
				}
				stateObj["transitions"] = std::move(transArray);
			}

			printf("unk flag1 at: 0x%08zX\n", reader.offset);
			// on_entry
			uint32_t onEntryCount = reader.readVarint();
			if (onEntryCount > 0) {
				json entryArray = json::array();
				entryArray.get_ref<json::array_t&>().reserve(onEntryCount);
				for (uint32_t j = 0; j < onEntryCount; j++) {
					entryArray.push_back(reader.readString());
				}
				if (verbose) {
					debugOnEntry = entryArray;
				}
				stateObj["on_entry"] = std::move(entryArray);
			}

			//  on_exit
			uint32_t onExitCount = reader.readVarint();
			if (onExitCount > 0) {
				json exitArray = json::array();
				exitArray.get_ref<json::array_t&>().reserve(onExitCount);
				for (uint32_t j = 0; j < onExitCount; j++) {
					exitArray.push_back(reader.readString());
				}
				if (verbose) {
					debugOnExit = exitArray;
				}
				stateObj["on_exit"] = std::move(exitArray);
			}

			// blend_transitions
			if (reader.readVarint() != 0) {
				float blendVal = reader.readFloat();
				stateObj["blend_transition"] = blendVal;
				debugBlendValue = blendVal;
			}
			else {
				uint32_t blend_transitions_count = reader.readVarint();
				if (blend_transitions_count > 0) {
					json blendObj = json::object();
					for (uint32_t j = 0; j < blend_transitions_count; j++) {
						float bk = reader.readFloat();
						float bv = reader.readFloat();
						// 因为 JSON 对象键只能是字符串，所以将 float 转换为 string
						blendObj[std::to_string(bk)] = bv;
					}
					if (verbose) {
						debugBlendTransitions = blendObj;
					}
					stateObj["blend_transitions"] = std::move(blendObj);
				}
			}

			// 6. 解析 blend_via_shortest_path
			if (reader.readVarint() != 0) {
				stateObj["blend_via_shortest_path"] = true;
				debugShortestPath = true;
			}


			//// VERSION==26不存在，>=28存在，解析 sound_effects
			if (m_format > 26) {
				uint32_t soundEffectsCount = reader.readVarint();
				if (soundEffectsCount > 0) {
					json soundEffectsArray = json::array();
					soundEffectsArray.get_ref<json::array_t&>().reserve(soundEffectsCount);
					for (uint32_t j = 0; j < soundEffectsCount; j++) {
						std::string effectName = reader.readString();
						json effectItem = json::object();
						effectItem["effect"] = std::move(effectName);
						soundEffectsArray.push_back(std::move(effectItem));
					}
					stateObj["sound_effects"] = std::move(soundEffectsArray);
				}
			}




			printf("end at: 0x%08zX\n", reader.offset);
			log_state_block(stateName,
				verbose && !debugAnimations.empty() ? &debugAnimations : nullptr,
				verbose && !debugTransitions.empty() ? &debugTransitions : nullptr,
				verbose && !debugOnEntry.empty() ? &debugOnEntry : nullptr,
				verbose && !debugOnExit.empty() ? &debugOnExit : nullptr,
				debugBlendValue,
				verbose && !debugBlendTransitions.empty() ? &debugBlendTransitions : nullptr,
				debugShortestPath);

			// 将当前 state 添加到 states 列表
			statesData[stateName] = std::move(stateObj);
		}

		// 挂载 states 并存入 controller 列表
		controllerData["states"] = std::move(statesData);
		controllers[animName] = std::move(controllerData);
	}

	root["animation_controllers"] = std::move(controllers);

	std::string result;
	if (this->isFormatJson())
	{
		result = root.dump(4, ' ', false);
	}
	else
	{
		result = root.dump(-1);
	}
	return bytesFromString(result);
}

void YSMParserV3::ParseTextureFiles(BufferReader& reader)
{
	reader.setContext("ParseTextureFiles");
	uint32_t cnt = reader.readVarint();
	m_textureFiles.reserve(m_textureFiles.size() + cnt);
	for (uint32_t i = 0; i < cnt; i++)
	{
		std::string name = reader.readString();
		std::string hash = reader.readString();
		printf("Reading Image: %s, %s\n", name.c_str(), hash.c_str());
		std::vector<uint8_t> fileData = reader.readByteSequence();
		uint32_t width, height;
		width = reader.readVarint();
		height = reader.readVarint();
		printf("Finish Texture at: 0x%08zX\n", reader.offset);

		uint32_t image_format = reader.readVarint();
		uint32_t unk_flag = reader.readVarint();
		Images::validateImageMetadata(name, reader.offset, image_format, unk_flag);

		uint32_t subTextureSize = reader.readVarint();
		for (uint32_t i = 0; i < subTextureSize; i++) {
			uint32_t specular_type = reader.readVarint(); // 1为NORMAL，2为高光
			auto specialImageData = ParseSpecialImage(reader);
			std::string suffix = (specular_type == 1) ? "_normal" : (specular_type == 2 ? "_specular" : "_special");
			appendFile(m_specialImageFiles, name + suffix, std::move(specialImageData));

			uint32_t sp_w = reader.readVarint();
			uint32_t sp_h = reader.readVarint();
			uint32_t sp_format = reader.readVarint();
			uint32_t sp_flag = reader.readVarint();
			Images::validateImageMetadata(name + suffix, reader.offset, sp_format, sp_flag);
		}

		appendFile(m_textureFiles, std::move(name), std::move(fileData));
	}
}

void YSMParserV3::deserialize(const uint8_t* bufferData, size_t size) {
	BufferReader reader{ bufferData, size, 0 };
	uint32_t version = reader.readDword();
	if (version != m_format) throw new ParserUnSupportVersionException();
	if (m_format < 4) {
		deserializeLegacyV1(reader);
	}
	else if (m_format <= 15) {
		deserializeLegacyV15(reader);
	}
	else {
		deserializeModern(reader);
	}
}

void YSMParserV3::deserializeLegacyV1(BufferReader& reader) {
	reader.setContext("deserializeLegacyV1");
	uint32_t unk_needSkipBytes = reader.readVarint();
	reader.offset += unk_needSkipBytes;

	uint32_t modelCount = reader.readVarint();
	std::vector<uint32_t> modelIds;
	modelIds.reserve(modelCount);
	m_modelFiles.reserve(m_modelFiles.size() + modelCount);
	for (uint32_t i = 0; i < modelCount; ++i) {
		uint32_t modelId = reader.readVarint();
		modelIds.push_back(modelId);
		if (reader.readVarint() != 1) throw ParserUnknownField();
		auto model = ParseModels(reader);

		std::string modelName;
		if (modelId == 1) modelName = "main";
		else if (modelId == 2) modelName = "arm";
		else if (modelId == 3) modelName = "arrow";
		else throw ParserUnknownField();
		appendFile(m_modelFiles, std::move(modelName), std::move(model));


		printf("Parse Models finish. 0x%08zX\n", reader.offset);
	}


	uint32_t animationBlobCount = reader.readVarint();
	std::vector<uint32_t> animIds;
	animIds.reserve(animationBlobCount);
	m_animationFiles.reserve(m_animationFiles.size() + animationBlobCount);
	for (uint32_t i = 0; i < animationBlobCount; ++i) {
		uint32_t modelId = reader.readVarint();
		animIds.push_back(modelId);
		reader.readVarint();
		auto anim = ParseAnimations(reader);
		appendFile(m_animationFiles, animationNameForModelId(modelId, false), std::move(anim));
	}


	printf("Parse Animations finish. 0x%08zX\n", reader.offset);

	uint32_t customTextureCount = reader.readVarint();
	m_textureFiles.reserve(m_textureFiles.size() + customTextureCount);
	for (uint32_t i = 0; i < customTextureCount; ++i) {
		std::string textureName = reader.readString();
		printf("texture %s at 0x%08zX\n", textureName.c_str(), reader.offset);
		if (textureName == "/ARROW\\") {
			textureName = "arrow";
		}

		if (m_format < 4) {
			uint32_t unkk = reader.readVarint(); if (unkk != 0x01) throw ParserUnknownField();
		}

		std::vector<uint8_t> fileData = reader.readByteSequence();
		uint32_t width = reader.readVarint();
		uint32_t height = reader.readVarint();
		auto pngBytes = encodeRgbaToPngMemory(fileData, width, height);
		if (pngBytes.empty()) {
			throw ParserUnknownField();
		}
		appendFile(m_textureFiles, std::move(textureName), std::move(pngBytes));
	}

	printf("Parse Textures finish. 0x%08zX\n", reader.offset);


	uint32_t modelTableSize = reader.readVarint();
	for (uint32_t i = 0; i < modelTableSize; ++i) {
		uint32_t modelId = reader.readVarint();
		std::string modelHash = reader.readString();
		auto it = std::find(modelIds.begin(), modelIds.end(), modelId);
		if (it != modelIds.end()) {
			size_t idx = std::distance(modelIds.begin(), it);
			if (idx < m_modelFiles.size()) {
				// m_modelFiles[idx].first = modelHash;
			}
		}
	}

	uint32_t animationsTableSize = reader.readVarint();
	for (uint32_t i = 0; i < animationsTableSize; ++i) {
		uint32_t animationId = reader.readVarint();
		std::string animationHash = reader.readString();
		auto it = std::find(animIds.begin(), animIds.end(), animationId);
		if (it != animIds.end()) {
			size_t idx = std::distance(animIds.begin(), it);
			if (idx < m_animationFiles.size()) {
				// m_animationFiles[idx].first = animationHash;
			}
		}
	}

	uint32_t textureTableSize = reader.readVarint();
	for (uint32_t i = 0; i < textureTableSize; ++i) {
		std::string textureName = reader.readString();
		std::string textureHash = reader.readString();
		bool matched = false;
		for (auto& item : m_textureFiles) {
			if (item.first == textureName) {
				// item.first = textureHash;
				matched = true;
				break;
			}
		}
		if (!matched && i < m_textureFiles.size()) {
			// m_textureFiles[i].first = textureHash;
		}
	}


	printf("FINISH ALL 0x%08zX\n", reader.offset);

}

void YSMParserV3::deserializeLegacyV15(BufferReader& reader) {
	reader.setContext("deserializeLegacyV15");
	uint32_t unk_needSkipBytes = reader.readVarint();
	reader.offset += unk_needSkipBytes;

	uint32_t modelCount = reader.readVarint();
	std::vector<uint32_t> modelIds;
	modelIds.reserve(modelCount);
	m_modelFiles.reserve(m_modelFiles.size() + modelCount);
	for (uint32_t i = 0; i < modelCount; ++i) {
		uint32_t modelId = reader.readVarint();
		modelIds.push_back(modelId);
		reader.readVarint(); // unk

		auto model = ParseModels(reader);

		std::string modelName;
		if (modelId == 1) modelName = "main";
		else if (modelId == 2) modelName = "arm";
		else if (modelId == 3) modelName = "arrow";
		else throw ParserUnknownField();
		appendFile(m_modelFiles, std::move(modelName), std::move(model));
	}

	uint32_t animationBlobCount = reader.readVarint();
	std::vector<uint32_t> animIds;
	animIds.reserve(animationBlobCount);
	m_animationFiles.reserve(m_animationFiles.size() + animationBlobCount);
	for (uint32_t i = 0; i < animationBlobCount; ++i) {
		uint32_t modelId = reader.readVarint();
		animIds.push_back(modelId);
		reader.readVarint();
		auto anim = ParseAnimations(reader);
		appendFile(m_animationFiles, animationNameForModelId(modelId, true), std::move(anim));
	}

	if (m_format > 9) {
		printf("start Controllers 0x%08zX\n", reader.offset);

		uint32_t animControllerCount = reader.readVarint();
		m_animControllerFiles.reserve(m_animControllerFiles.size() + animControllerCount);
		for (uint32_t i = 0; i < animControllerCount; i++)
		{
			// 全局控制器的名称和 hash
			std::string controllerName;
			std::string hash;
			if (m_format <= 15) {
				controllerName = "controller";
				reader.readVarint();
			}
			else {
				controllerName = reader.readString();
				hash = reader.readString();
			}
			auto animController = ParseAnimationControllers(reader);
			appendFile(m_animControllerFiles, std::move(controllerName), std::move(animController));
		}

		uint32_t animationControllerTableSize = reader.readVarint();
		for (uint32_t i = 0; i < animationControllerTableSize; ++i) {
			std::string controllerName = reader.readString();
			std::string controllerHash = reader.readString();
			if (i < m_animControllerFiles.size()) {
				// m_animControllerFiles[i].first = controllerHash;
			}
		}
	}

	printf("Reading Textures. 0x%08zX\n", reader.offset);
	uint32_t customTextureCount = reader.readVarint();
	m_textureFiles.reserve(m_textureFiles.size() + customTextureCount);
	for (uint32_t i = 0; i < customTextureCount; ++i) {
		std::string textureName = reader.readString();
		if (textureName == "/ARROW\\") {
			textureName = "arrow";
		}
		std::vector<uint8_t> fileData = reader.readByteSequence();
		uint32_t width = reader.readVarint();
		uint32_t height = reader.readVarint();
		auto pngBytes = encodeRgbaToPngMemory(fileData, width, height);
		if (pngBytes.empty()) {
			throw ParserUnknownField();
		}
		appendFile(m_textureFiles, textureName, std::move(pngBytes));
		printf("sub texture 0x%08zX\n", reader.offset);

		uint32_t subTextureSize = reader.readVarint();
		for (uint32_t i = 0; i < subTextureSize; i++) {
			uint32_t specular_type = reader.readVarint(); // 1为NORMAL，2为高光

			std::vector<uint8_t> fileData = reader.readByteSequence();
			uint32_t width = reader.readVarint();
			uint32_t height = reader.readVarint();
			auto pngBytes = encodeRgbaToPngMemory(fileData, width, height);
			if (pngBytes.empty()) {
				throw ParserUnknownField();
			}
			std::string suffix = (specular_type == 1) ? "_normal" : (specular_type == 2 ? "_specular" : "_special");
			appendFile(m_specialImageFiles, textureName + suffix, std::move(pngBytes));
		}
	}

	printf("Finish Textures. 0x%08zX\n", reader.offset);

	if (m_format > 9) {
		ParseSoundFiles(reader);

		printf("Finish Sounds 0x%08zX\n", reader.offset);

		uint32_t soundTableCount = reader.readVarint();
		for (uint32_t i = 0; i < soundTableCount; ++i) {
			std::string soundName = reader.readString();
			std::string soundHash = reader.readString();
			for (auto& item : m_soundFiles) {
				if (item.first == soundName) {
					// item.first = soundHash;
					break;
				}
			}
		}
	}

	uint32_t extraTextureCount = reader.readVarint();
	m_avatarFiles.reserve(m_avatarFiles.size() + extraTextureCount);
	for (uint32_t i = 0; i < extraTextureCount; ++i) {
		std::string textureName = reader.readString();
		std::vector<uint8_t> fileData = reader.readByteSequence();
		uint32_t width = reader.readVarint();
		uint32_t height = reader.readVarint();
		auto pngBytes = encodeRgbaToPngMemory(fileData, width, height);
		if (pngBytes.empty()) {
			throw ParserUnknownField();
		}
		appendFile(m_avatarFiles, std::move(textureName), std::move(pngBytes));
	}

	uint32_t modelTableSize = reader.readVarint();
	for (uint32_t i = 0; i < modelTableSize; ++i) {
		uint32_t modelId = reader.readVarint();
		std::string modelHash = reader.readString();
		auto it = std::find(modelIds.begin(), modelIds.end(), modelId);
		if (it != modelIds.end()) {
			size_t idx = std::distance(modelIds.begin(), it);
			if (idx < m_modelFiles.size()) {
				// m_modelFiles[idx].first = modelHash;
			}
		}
	}

	uint32_t animationsTableSize = reader.readVarint();
	for (uint32_t i = 0; i < animationsTableSize; ++i) {
		uint32_t animationId = reader.readVarint();
		std::string animationHash = reader.readString();
		auto it = std::find(animIds.begin(), animIds.end(), animationId);
		if (it != animIds.end()) {
			size_t idx = std::distance(animIds.begin(), it);
			if (idx < m_animationFiles.size()) {
				// m_animationFiles[idx].first = animationHash;
			}
		}
	}

	size_t specialImageIdx = 0;
	uint32_t textureTableSize = reader.readVarint();
	for (uint32_t i = 0; i < textureTableSize; ++i) {
		std::string textureName = reader.readString();
		std::string textureHash = reader.readString();
		bool matched = false;
		for (auto& item : m_textureFiles) {
			if (item.first == textureName) {
				// item.first = textureHash;
				matched = true;
				break;
			}
		}
		if (!matched && i < m_textureFiles.size()) {
			// m_textureFiles[i].first = textureHash;
		}

		printf("Ready for read 0x%08zX\n", reader.offset);
		uint32_t subTextureSize = reader.readVarint();
		printf("Finish [%d] 0x%08zX\n", subTextureSize, reader.offset);

		for (uint32_t i = 0; i < subTextureSize; i++) {
			uint32_t specular_type = reader.readVarint(); // 1为NORMAL，2为高光
			std::string textureHash = reader.readString();

			if (specialImageIdx < m_specialImageFiles.size()) {
				// m_specialImageFiles[specialImageIdx].first = textureHash;
				specialImageIdx++;
			}
		}
	}


	printf("Finish All Tabel 0x%08zX\n", reader.offset);

	ParseYSMJson(reader);
}

void YSMParserV3::deserializeModern(BufferReader& reader) {
	reader.setContext("deserializeModern");
	// Sound
	ParseSoundFiles(reader);
	printf("Parse Sound finish. 0x%08zX\n", reader.offset);

	// Function
	ParseFunctionFiles(reader);
	printf("Parse Function finish. 0x%08zX\n", reader.offset);

	// language
	ParseLanguageFiles(reader);
	printf("Parse Language finish. 0x%08zX\n", reader.offset);

	auto parseSubEntity = [&](BufferReader& reader, const std::string& categoryName, uint32_t index) {
		printf("--- Parsing %s [%u] at 0x%08zX ---\n", categoryName.c_str(), index, reader.offset);

		printf("Ready for read string 0x%08zX\n", reader.offset);
		std::string subModuleName;
		if (m_format <= 26) {
			subModuleName = reader.readString();
			printf("  -> SubModule Name (Header): %s\n", subModuleName.c_str());
		}
		printf("Ready for join 0x%08zX\n", reader.offset);
		// 1. 解析动画
		std::vector<uint8_t> anim;
		bool hasSubAnim = reader.readVarint();
		if (hasSubAnim)
		{
			anim = ParseAnimations(reader);
		}

		// 子模型Controller
		bool hasSubController = reader.readVarint();
		std::vector<std::pair<std::string, std::vector<uint8_t>>> subTextureFiles;
		std::vector<uint8_t> basicTexture, subController;
		if (hasSubController)
		{
			std::string controllerHash = reader.readString();
			// Single Controller
			subController = ParseAnimationControllers(reader);
		}

		// 2. 解析基础 UV 纹理
		basicTexture = ParseSpecialImage(reader);
		uint32_t uv_w = reader.readVarint();
		uint32_t uv_h = reader.readVarint();
		uint32_t uv_format = reader.readVarint();
		uint32_t uv_unk2 = reader.readVarint(); // 通常是 1

		// 子纹理大小

		uint32_t subTextureSize = reader.readVarint();
		subTextureFiles.reserve(subTextureSize);
		for (uint32_t i = 0; i < subTextureSize; i++) {
			uint32_t specular_type = reader.readVarint(); // 1为NORMAL，2为高光(spec)
			auto basicSubTexture = ParseSpecialImage(reader);
			if (specular_type == 1)
			{
				appendFile(subTextureFiles, "normal", std::move(basicSubTexture));
			}
			else if (specular_type == 2)
			{
				appendFile(subTextureFiles, "specular", std::move(basicSubTexture));
			}
			else
			{
				throw ParserUnknownField();
			}

			uint32_t sp_w = reader.readVarint();
			uint32_t sp_h = reader.readVarint();
			uint32_t sp_format = reader.readVarint();
			uint32_t sp_unk2 = reader.readVarint();
		}


		// 4. 解析几何模型
		auto model = ParseModels(reader);

		// 5. 解析实体尾部数据
		if (m_format > 26) {
			// 多种submodels处理
			uint32_t subModels = reader.readVarint();
			for (uint32_t j = 0; j < subModels; j++)
			{
				subModuleName = reader.readString();
				printf("  -> SubModule Name (Footer): %s\n", subModuleName.c_str());
				if (subModuleName.find("minecraft:") != std::string::npos) subModuleName = subModuleName.substr(subModuleName.find(":") + 1);
				m_subEntityCategories[subModuleName] = categoryName;
				appendFile(m_modelFiles, subModuleName, model);
				for (auto& item : subTextureFiles) {
					appendFile(m_textureFiles, subModuleName + "_" + item.first, item.second);
				}
				appendFile(m_textureFiles, subModuleName, basicTexture);
				if (hasSubAnim)
				{
					appendFile(m_animationFiles, categoryName + "/" + subModuleName, anim);
				}
				if (hasSubController)
				{
					appendFile(m_animControllerFiles, categoryName + "/" + subModuleName, subController);
				}
				
			}
			return;
		}
		if (subModuleName.find("minecraft:") != std::string::npos) subModuleName = subModuleName.substr(subModuleName.find(":") + 1);
		m_subEntityCategories[subModuleName] = categoryName;
		appendFile(m_modelFiles, subModuleName, std::move(model));
		for (auto& item : subTextureFiles) {
			appendFile(m_textureFiles, subModuleName + "_" + item.first, std::move(item.second));
		}
		appendFile(m_textureFiles, subModuleName, std::move(basicTexture));
		if (hasSubAnim)
		{
			appendFile(m_animationFiles, categoryName + "/" + subModuleName, std::move(anim));
		}
		if (hasSubController)
		{
			appendFile(m_animControllerFiles, categoryName + "/" + subModuleName, std::move(subController));
		}

		};

	// 2. 开始执行结构化读取

	if (m_format < 26) {
		// <=26 版本只存在一种 SubEntity
		uint32_t subEntitySize = reader.readVarint();
		printf("\nTotal SubEntities: %u\n", subEntitySize);
		for (uint32_t i = 0; i < subEntitySize; i++) {
			parseSubEntity(reader, "SubEntity", i);
		}
		uint32_t footerFlag = reader.readVarint(); // 通常是 0x01
	}
	else {
		{
			// 读取载具 (Vehicles)
			uint32_t vehiclesSize = reader.readVarint();
			printf("\nTotal Vehicles: %u\n", vehiclesSize);
			for (uint32_t i = 0; i < vehiclesSize; i++) {
				parseSubEntity(reader, "vehicle", i);
			}
		}

		{
			// 读取投掷物/弹射物 (Projectiles)
			uint32_t projectileSize = reader.readVarint();
			printf("\nTotal Projectiles: %u\n", projectileSize);
			for (uint32_t i = 0; i < projectileSize; i++) {
				parseSubEntity(reader, "projectiles", i);
			}
		}
	}

	if (reader.readVarint() != 1) throw ParserUnknownField(); // TODO:这个1 是什么？

	// animations
	uint32_t cnt = reader.readVarint();
	m_animationFiles.reserve(m_animationFiles.size() + cnt);
	for (uint32_t i = 0; i < cnt; i++)
	{
		uint32_t modelId = reader.readVarint();
		auto anim = ParseAnimations(reader);
		appendFile(m_animationFiles, animationNameForModelId(modelId, false), std::move(anim));
	}

	// animations-controller
	uint32_t animControllerCount = reader.readVarint();
	m_animControllerFiles.reserve(m_animControllerFiles.size() + animControllerCount);
	for (uint32_t i = 0; i < animControllerCount; i++)
	{
		// 全局控制器的名称和 hash
		std::string controllerName;
		std::string hash;
		if (m_format <= 15) {
			controllerName = "controller";
			reader.readVarint();
		}
		else {
			controllerName = reader.readString();
			hash = reader.readString();
		}
		auto animController = ParseAnimationControllers(reader);
		appendFile(m_animControllerFiles, std::move(controllerName), std::move(animController));
	}

	// Textures
	ParseTextureFiles(reader);

	uint32_t modelSize = reader.readVarint();
	m_modelFiles.reserve(m_modelFiles.size() + modelSize);
	for (uint32_t i = 0; i < modelSize; i++) {
		uint32_t modelId = reader.readVarint();
		auto model = ParseModels(reader);
		std::string modelName;
		if (modelId == 1) modelName = "main";
		else if (modelId == 2) modelName = "arm";
		else throw ParserUnknownField();
		appendFile(m_modelFiles, std::move(modelName), std::move(model));
	}
	printf("finish models: 0x%08zX\n", reader.offset);

	ParseYSMJson(reader);
}

// 辅助函数：将单个十六进制字符转换为对应的 0-15 的整数值
constexpr uint8_t hexCharToValue(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	throw std::invalid_argument(std::string("Invalid hex character: ") + c);
}

// 主函数：将十六进制字符串转换为字节数组
static std::vector<uint8_t> hexStrToBytes(std::string_view hex) {
	// 十六进制字符串的长度必须是偶数，因为两个字符表示一个字节
	if (hex.length() % 2 != 0) {
		throw std::invalid_argument("Hex string length must be even");
	}

	std::vector<uint8_t> bytes;
	// 预分配内存，避免频繁的动态扩容
	bytes.reserve(hex.length() / 2);

	for (size_t i = 0; i < hex.length(); i += 2) {
		// 提取高位(high nibble)和低位(low nibble)
		uint8_t high = hexCharToValue(hex[i]);
		uint8_t low = hexCharToValue(hex[i + 1]);

		// 组合成一个完整的字节并存入 vector
		bytes.push_back((high << 4) | low);
	}

	return bytes;
}


void YSMParserV3::parse()
{
	using namespace MemoryUtils;
	static std::once_flag fpng_init_flag;
	clearProfileEntries();
	m_binaryData.clear();
	m_decrypted.clear();
	m_decompressed.clear();
	auto ip = m_buffer.get();
	std::string header = ip;
	ScopedOstreamStateSilencer quietStdout(std::cout, !isVerbose());

	std::call_once(fpng_init_flag, []() {
		fpng::fpng_init();
		});

	// Parse Header
	try {
		m_format = extractFormatFromHeader(header);
	}
	catch (const std::exception& e) {
		if (isVerbose()) {
			std::cout << "Parse Failed: " << e.what() << std::endl;
		}
		throw ParserInvalidFileFormatException();
	}


	// Size < mininal_header + len(dyKey + iv + fileHash)
	if (m_size < 8 + 24 + 32 + 8) throw ParserInvalidFileFormatException();

	// Read Key, IV and FileHash from the end of the file
	char* ptrFileTail = ip + m_size - 64;
	static_assert(sizeof(m_key) + sizeof(m_iv) + sizeof(m_fileHash) == 64, "Tail data size mismatch");
	std::copy(ptrFileTail, ptrFileTail + m_key.size(), m_key.begin());
	ptrFileTail += m_key.size();
	std::copy(ptrFileTail, ptrFileTail + m_iv.size(), m_iv.begin());
	ptrFileTail += m_iv.size();
	m_fileHash = readLE<uint64_t>(ptrFileTail, 0);

	// Parse binaryData
	char* ptrBinaryData = ip + header.size() + 1;
	uint32_t crypto = readLE<uint32_t>(ptrBinaryData, 0);
	if (crypto != 3) throw ParserInvalidFileFormatException();
	ptrBinaryData += sizeof(uint32_t);

	// Verify File
	auto stage_start = std::chrono::steady_clock::now();
	uint64_t file_hash = CityHash64WithSeed(
		reinterpret_cast<const char*>(ip),
		(m_size - sizeof(m_fileHash)),
		SEED_FILE_VERIFICATION
	);
	recordProfileStage(
		"file_verify",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage_start).count()
	);
	if (file_hash != m_fileHash) throw ParserCorruptedDataException();

	auto* binaryBegin = reinterpret_cast<const uint8_t*>(ptrBinaryData);
	auto* binaryEnd = reinterpret_cast<const uint8_t*>(ip + m_size - 64);
	if (binaryEnd < binaryBegin) throw ParserInvalidFileFormatException();
	std::span<const uint8_t> binaryData(binaryBegin, static_cast<std::size_t>(binaryEnd - binaryBegin));
	if (isDebug()) {
		m_binaryData.assign(binaryData.begin(), binaryData.end());
	}

	// Decrypt BinaryData
	stage_start = std::chrono::steady_clock::now();
	std::vector<uint8_t> chacha_decrypted = CryptoUtils::ModifiedChaChaDecrypt(binaryData, m_key.data(), m_iv.data(), SEED_RES_VERIFICATION);
	recordProfileStage(
		"chacha_decrypt",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage_start).count()
	);

	stage_start = std::chrono::steady_clock::now();
	CryptoUtils::MT19937XorInPlace(std::span<uint8_t>(chacha_decrypted.data(), chacha_decrypted.size()), m_key.data(), m_iv.data());
	recordProfileStage(
		"mt19937_xor",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage_start).count()
	);

	// Parse Decrypted Data
	// First two bytes is Nonce length
	stage_start = std::chrono::steady_clock::now();
	if (chacha_decrypted.size() < 2) throw ParserInvalidFileFormatException();
	uint16_t n = static_cast<uint16_t>(chacha_decrypted[0]) | (static_cast<uint16_t>(chacha_decrypted[1]) << 8);
	n &= 0x3ff;
	// Next Sequence is trueData
	if (2u + n > chacha_decrypted.size()) throw ParserInvalidFileFormatException();
	std::span<const uint8_t> compressedData(
		chacha_decrypted.data() + 2 + n,
		chacha_decrypted.size() - 2 - n
	);
	if (isDebug()) {
		m_decrypted.assign(compressedData.begin(), compressedData.end());
	}
	recordProfileStage(
		"nonce_trim",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage_start).count()
	);

	// m_decrypted need to be decompressed by ZSTD.


	stage_start = std::chrono::steady_clock::now();
	m_decompressed = CryptoUtils::DecompressZstd(compressedData);
	recordProfileStage(
		"zstd_decompress",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage_start).count()
	);


	// Start Parse Files
	if (isVerbose()) {
		std::cout << "Start Parse Files (format = " << m_format << ")\n";
	}

	stage_start = std::chrono::steady_clock::now();
	deserialize(m_decompressed.data(), m_decompressed.size());
	recordProfileStage(
		"deserialize",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stage_start).count()
	);
}

static void saveFile(const std::filesystem::path& filePath, const std::vector<uint8_t>& data, bool verbose) {
	// 直接使用 filesystem::path，确保 Windows 下由宽字符路径打开。
	if (filePath.has_parent_path()) {
		std::filesystem::create_directories(filePath.parent_path());
	}

	// 2. 直接把 path 对象传给 ofstream
	// 在 Windows 下，这会调用 _wstring 版本的打开函数，彻底解决路径乱码
	std::ofstream outFile(filePath, std::ios::out | std::ios::binary);

	if (outFile.is_open()) {
		outFile.write(reinterpret_cast<const char*>(data.data()), data.size());
		outFile.close();

		if (verbose) {
			std::cout << "[EXPORT] " << PathUtils::path_to_utf8(filePath)
				<< "  (" << data.size() << " bytes)" << std::endl;
		}
	}
	else {
		std::cerr << "Failed to open file: " << PathUtils::path_to_utf8(filePath) << std::endl;
		throw ParserPathNotSupported();
	}
}

void YSMParserV3::saveToDirectory(std::string output_directory)
{
	namespace fs = std::filesystem;
	const bool useLegacyRootLayout = m_ysmJsonFile.empty();

	fs::path dirPath = PathUtils::utf8_to_path(output_directory);
	if (!fs::exists(dirPath)) {
		fs::create_directories(dirPath);
	}


	auto resolveRelativePath = [&](const fs::path& relativePath) -> fs::path {
		return useLegacyRootLayout ? relativePath.filename() : relativePath;
		};

	auto makeOutputPath = [&](const fs::path& relativePath) -> fs::path {
		return dirPath / resolveRelativePath(relativePath);
		};

	if (useLegacyRootLayout) {
		if (!m_infoJsonFile.empty()) {
			saveFile(dirPath / "info.json", m_infoJsonFile, isVerbose());
		}
	}
	else if (!m_ysmJsonFile.empty()) {
		saveFile(dirPath / "ysm.json", m_ysmJsonFile, isVerbose());
	}

	if (isDebug()) {
		saveFile(dirPath / "_debug_m_decompressed.bin", m_decompressed, isVerbose());
		saveFile(dirPath / "_debug_m_decrypted.bin", m_decrypted, isVerbose());
		saveFile(dirPath / "_debug_m_binaryData.bin", m_binaryData, isVerbose());
	}

	auto exportMapped = [&](const std::vector<std::pair<std::string, std::vector<uint8_t>>>& items, const fs::path& defaultFolder, const std::string& extension) {
		for (const auto& item : items) {
			std::string rawPath = item.first;

			if (!rawPath.ends_with(extension)) {
				rawPath += extension;
			}

			fs::path p(PathUtils::utf8_to_path(rawPath));
			std::string safeFilename = sanitizeWindowsFilename(PathUtils::path_to_utf8(p.filename()));
			fs::path safeRelativePath = p.parent_path() / PathUtils::utf8_to_path(safeFilename);

			auto fallbackPath = makeOutputPath(defaultFolder / safeRelativePath);
			saveFile(fallbackPath, item.second, isVerbose());
		}
		};

	exportMapped(m_soundFiles, "sounds", ".ogg");
	exportMapped(m_functionFiles, "functions", ".molang");
	exportMapped(m_languageFiles, "lang", ".json");
	exportMapped(m_animControllerFiles, "controller", ".json");
	exportMapped(m_modelFiles, "models", ".json");
	exportMapped(m_animationFiles, "animations", ".animation.json");
	exportMapped(m_textureFiles, "textures", ".png");
	exportMapped(m_specialImageFiles, "textures", ".png");

	for (auto it = m_avatarFiles.begin(); it != m_avatarFiles.end(); ++it) {
		std::string safeFilename = sanitizeWindowsFilename(it->first + ".png");
		fs::path outPath = makeOutputPath(fs::path("avatar") / PathUtils::utf8_to_path(safeFilename));
		saveFile(outPath, it->second, isVerbose());
	}

	for (const auto& item : m_backgroundFiles) {
		std::string relativePath = item.first;

		if (!PathUtils::utf8_to_path(relativePath).has_extension()) {
			relativePath += ".png";
		}

		fs::path p(PathUtils::utf8_to_path(relativePath));
		std::string safeFilename = sanitizeWindowsFilename(PathUtils::path_to_utf8(p.filename()));
		fs::path safeRelativePath = p.parent_path() / PathUtils::utf8_to_path(safeFilename);

		fs::path outPath = makeOutputPath(safeRelativePath);
		saveFile(outPath, item.second, isVerbose());
	}
}
