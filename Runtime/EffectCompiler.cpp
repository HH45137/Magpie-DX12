#include "pch.h"
#include "EffectCompiler.h"
#include <unordered_set>
#include "Utils.h"
#include <bitset>
#include <charconv>
#include "EffectCache.h"
#include "StrUtils.h"
#include "App.h"
#include "DeviceResources.h"
#include "Logger.h"


static constexpr const char* META_INDICATOR = "//!";


class PassInclude : public ID3DInclude {
public:
	HRESULT CALLBACK Open(
		D3D_INCLUDE_TYPE IncludeType,
		LPCSTR pFileName,
		LPCVOID pParentData,
		LPCVOID* ppData,
		UINT* pBytes
	) override {
		std::wstring relativePath = L"effects\\" + StrUtils::UTF8ToUTF16(pFileName);

		std::string file;
		if (!Utils::ReadTextFile(relativePath.c_str(), file)) {
			return E_FAIL;
		}

		char* result = new char[file.size()];
		std::memcpy(result, file.data(), file.size());

		*ppData = result;
		*pBytes = (UINT)file.size();

		return S_OK;
	}

	HRESULT CALLBACK Close(LPCVOID pData) override {
		delete[](char*)pData;
		return S_OK;
	}
};

static PassInclude passInclude;

UINT RemoveComments(std::string& source) {
	// 确保以换行符结尾
	if (source.back() != '\n') {
		source.push_back('\n');
	}

	std::string result;
	result.reserve(source.size());

	int j = 0;
	// 单独处理最后两个字符
	for (size_t i = 0, end = source.size() - 2; i < end; ++i) {
		if (source[i] == '/') {
			if (source[i + 1] == '/' && source[i + 2] != '!') {
				// 行注释
				i += 2;

				// 无需处理越界，因为必定以换行符结尾
				while (source[i] != '\n') {
					++i;
				}

				// 保留换行符
				source[j++] = '\n';

				continue;
			} else if (source[i + 1] == '*') {
				// 块注释
				i += 2;

				while (true) {
					if (++i >= source.size()) {
						// 未闭合
						return 1;
					}

					if (source[i - 1] == '*' && source[i] == '/') {
						break;
					}
				}

				// 文件结尾
				if (i >= source.size() - 2) {
					source.resize(j);
					return 0;
				}

				continue;
			}
		}

		source[j++] = source[i];
	}

	// 无需复制最后的换行符
	source[j++] = source[source.size() - 2];
	source.resize(j);
	return 0;
}

template<bool IncludeNewLine>
static void RemoveLeadingBlanks(std::string_view& source) {
	size_t i = 0;
	for (; i < source.size(); ++i) {
		if constexpr (IncludeNewLine) {
			if (!StrUtils::isspace(source[i])) {
				break;
			}
		} else {
			char c = source[i];
			if (c != ' ' && c != '\t') {
				break;
			}
		}
	}

	source.remove_prefix(i);
}

template<bool AllowNewLine>
static bool CheckNextToken(std::string_view& source, std::string_view token) {
	RemoveLeadingBlanks<AllowNewLine>(source);

	if (!source.starts_with(token)) {
		return false;
	}

	source.remove_prefix(token.size());
	return true;
}

template<bool AllowNewLine>
static UINT GetNextToken(std::string_view& source, std::string_view& value) {
	RemoveLeadingBlanks<AllowNewLine>(source);

	if (source.empty()) {
		return 2;
	}

	char cur = source[0];

	if (StrUtils::isalpha(cur) || cur == '_') {
		size_t j = 1;
		for (; j < source.size(); ++j) {
			cur = source[j];

			if (!StrUtils::isalnum(cur) && cur != '_') {
				break;
			}
		}

		value = source.substr(0, j);
		source.remove_prefix(j);
		return 0;
	}

	if constexpr (AllowNewLine) {
		return 1;
	} else {
		return cur == '\n' ? 2 : 1;
	}
}

bool CheckMagic(std::string_view& source) {
	std::string_view token;
	if (!CheckNextToken<true>(source, META_INDICATOR)) {
		return false;
	}

	if (!CheckNextToken<false>(source, "MAGPIE")) {
		return false;
	}
	if (!CheckNextToken<false>(source, "EFFECT")) {
		return false;
	}

	if (GetNextToken<false>(source, token) != 2) {
		return false;
	}

	if (source.empty()) {
		return false;
	}

	return true;
}

UINT GetNextString(std::string_view& source, std::string_view& value) {
	RemoveLeadingBlanks<false>(source);
	size_t pos = source.find('\n');

	value = source.substr(0, pos);
	StrUtils::Trim(value);
	if (value.empty()) {
		return 1;
	}

	source.remove_prefix(std::min(pos + 1, source.size()));
	return 0;
}

template<typename T>
static UINT GetNextNumber(std::string_view& source, T& value) {
	RemoveLeadingBlanks<false>(source);

	if (source.empty()) {
		return 1;
	}

	const auto& result = std::from_chars(source.data(), source.data() + source.size(), value);
	if ((int)result.ec) {
		return 1;
	}

	// 解析成功
	source.remove_prefix(result.ptr - source.data());
	return 0;
}

UINT GetNextExpr(std::string_view& source, std::string& expr) {
	RemoveLeadingBlanks<false>(source);
	size_t size = std::min(source.find('\n') + 1, source.size());

	// 移除空白字符
	expr.resize(size);

	size_t j = 0;
	for (size_t i = 0; i < size; ++i) {
		char c = source[i];
		if (!isspace(c)) {
			expr[j++] = c;
		}
	}
	expr.resize(j);

	if (expr.empty()) {
		return 1;
	}

	source.remove_prefix(size);
	return 0;
}

UINT ResolveHeader(std::string_view block, EffectDesc& desc) {
	// 必需的选项：VERSION
	// 可选的选项：OUTPUT_WIDTH，OUTPUT_HEIGHT

	std::bitset<3> processed;

	std::string_view token;

	while (true) {
		if (!CheckNextToken<true>(block, META_INDICATOR)) {
			break;
		}

		if (GetNextToken<false>(block, token)) {
			return 1;
		}
		std::string t = StrUtils::ToUpperCase(token);

		if (t == "VERSION") {
			if (processed[0]) {
				return 1;
			}
			processed[0] = true;

			UINT version;
			if (GetNextNumber(block, version)) {
				return 1;
			}

			if (version != EffectCompiler::VERSION) {
				return 1;
			}

			if (GetNextToken<false>(block, token) != 2) {
				return 1;
			}
		} else if (t == "OUTPUT_WIDTH") {
			if (processed[1]) {
				return 1;
			}
			processed[1] = true;

			if (GetNextExpr(block, desc.outSizeExpr.first)) {
				return 1;
			}
		} else if (t == "OUTPUT_HEIGHT") {
			if (processed[2]) {
				return 1;
			}
			processed[2] = true;

			if (GetNextExpr(block, desc.outSizeExpr.second)) {
				return 1;
			}
		} else {
			return 1;
		}
	}

	// HEADER 块不含代码部分
	if (GetNextToken<true>(block, token) != 2) {
		return 1;
	}

	if (!processed[0] || (processed[1] ^ processed[2])) {
		return 1;
	}

	return 0;
}

UINT ResolveConstant(std::string_view block, EffectDesc& desc) {
	// 可选的选项：VALUE，DEFAULT，LABEL，MIN，MAX, DYNAMIC
	// VALUE 与其他选项互斥
	// 如果无 VALUE 则必须有 DEFAULT

	std::bitset<6> processed;

	std::string_view token;

	if (!CheckNextToken<true>(block, META_INDICATOR)) {
		return 1;
	}

	if (!CheckNextToken<false>(block, "CONSTANT")) {
		return 1;
	}
	if (GetNextToken<false>(block, token) != 2) {
		return 1;
	}

	EffectConstantDesc desc1{};
	EffectValueConstantDesc desc2{};

	std::string_view defaultValue;
	std::string_view minValue;
	std::string_view maxValue;

	while (true) {
		if (!CheckNextToken<true>(block, META_INDICATOR)) {
			break;
		}

		if (GetNextToken<false>(block, token)) {
			return 1;
		}

		std::string t = StrUtils::ToUpperCase(token);

		if (t == "VALUE") {
			for (int i = 0; i < 5; ++i) {
				if (processed[i]) {
					return 1;
				}
			}
			processed[0] = true;

			if (GetNextExpr(block, desc2.valueExpr)) {
				return 1;
			}
		} else if (t == "DEFAULT") {
			if (processed[0] || processed[1] || processed[5]) {
				return 1;
			}
			processed[1] = true;

			if (GetNextString(block, defaultValue)) {
				return 1;
			}
		} else if (t == "LABEL") {
			if (processed[0] || processed[2]) {
				return 1;
			}
			processed[2] = true;

			std::string_view t;
			if (GetNextString(block, t)) {
				return 1;
			}
			desc1.label = t;
		} else if (t == "MIN") {
			if (processed[0] || processed[3] || processed[5]) {
				return 1;
			}
			processed[3] = true;

			if (GetNextString(block, minValue)) {
				return 1;
			}
		} else if (t == "MAX") {
			if (processed[0] || processed[4] || processed[5]) {
				return 1;
			}
			processed[4] = true;

			if (GetNextString(block, maxValue)) {
				return 1;
			}
		} else if (t == "DYNAMIC") {
			for (int i = 1; i < 6; ++i) {
				if (processed[i]) {
					return 1;
				}
			}
			processed[5] = true;

			if (GetNextToken<false>(block, token) !=2) {
				return 1;
			}
		} else{
			return 1;
		}
	}

	// DYNAMIC 必须和 VALUE 一起出现
	if (!processed[0] && processed[5]) {
		return 1;
	}

	// VALUE 或 DEFAULT 必须存在
	if (processed[0] == processed[1]) {
		return 1;
	}

	// 代码部分
	if (GetNextToken<true>(block, token)) {
		return 1;
	}

	if (token == "float") {
		if (processed[0]) {
			desc2.type = EffectConstantType::Float;
		} else {
			desc1.type = EffectConstantType::Float;

			if (!defaultValue.empty()) {
				desc1.defaultValue = 0.0f;
				if (GetNextNumber(defaultValue, std::get<float>(desc1.defaultValue))) {
					return 1;
				}
			}
			if (!minValue.empty()) {
				float value;
				if (GetNextNumber(minValue, value)) {
					return 1;
				}

				if (!defaultValue.empty() && std::get<float>(desc1.defaultValue) < value) {
					return 1;
				}

				desc1.minValue = value;
			}
			if (!maxValue.empty()) {
				float value;
				if (GetNextNumber(maxValue, value)) {
					return 1;
				}

				if (!defaultValue.empty() && std::get<float>(desc1.defaultValue) > value) {
					return 1;
				}

				if (!minValue.empty() && std::get<float>(desc1.minValue) > value) {
					return 1;
				}

				desc1.maxValue = value;
			}
		}
	} else if (token == "int") {
		if (processed[0]) {
			desc2.type = EffectConstantType::Int;
		} else {
			desc1.type = EffectConstantType::Int;

			if (!defaultValue.empty()) {
				desc1.defaultValue = 0;
				if (GetNextNumber(defaultValue, std::get<int>(desc1.defaultValue))) {
					return 1;
				}
			}
			if (!minValue.empty()) {
				int value;
				if (GetNextNumber(minValue, value)) {
					return 1;
				}

				if (!defaultValue.empty() && std::get<int>(desc1.defaultValue) < value) {
					return 1;
				}

				desc1.minValue = value;
			}
			if (!maxValue.empty()) {
				int value;
				if (GetNextNumber(maxValue, value)) {
					return 1;
				}

				if (!defaultValue.empty() && std::get<int>(desc1.defaultValue) > value) {
					return 1;
				}

				if (!minValue.empty() && std::get<int>(desc1.minValue) > value) {
					return 1;
				}

				desc1.maxValue = value;
			}
		}
	} else {
		return 1;
	}

	if (GetNextToken<true>(block, token)) {
		return 1;
	}
	(processed[0] ? desc2.name : desc1.name) = token;

	if (!CheckNextToken<true>(block, ";")) {
		return 1;
	}

	if (GetNextToken<true>(block, token) != 2) {
		return 1;
	}

	if (processed[0]) {
		if (processed[5]) {
			desc.dynamicValueConstants.emplace_back(std::move(desc2));
		} else {
			desc.valueConstants.emplace_back(std::move(desc2));
		}
	} else {
		desc.constants.emplace_back(std::move(desc1));
	}

	return 0;
}


UINT ResolveTexture(std::string_view block, EffectDesc& desc) {
	// 如果名称为 INPUT 不能有任何选项，含 SOURCE 时不能有任何其他选项
	// 否则必需的选项：FORMAT
	// 可选的选项：WIDTH，HEIGHT

	EffectIntermediateTextureDesc& texDesc = desc.textures.emplace_back();

	std::bitset<4> processed;

	std::string_view token;

	if (!CheckNextToken<true>(block, META_INDICATOR)) {
		return 1;
	}

	if (!CheckNextToken<false>(block, "TEXTURE")) {
		return 1;
	}
	if (GetNextToken<false>(block, token) != 2) {
		return 1;
	}

	while (true) {
		if (!CheckNextToken<true>(block, META_INDICATOR)) {
			break;
		}

		if (GetNextToken<false>(block, token)) {
			return 1;
		}

		std::string t = StrUtils::ToUpperCase(token);

		if (t == "SOURCE") {
			if (processed.any()) {
				return 1;
			}
			processed[0] = true;

			if (GetNextString(block, token)) {
				return 1;
			}

			texDesc.source = token;
		} else if (t == "FORMAT") {
			if (processed[0] || processed[1]) {
				return 1;
			}
			processed[1] = true;

			if (GetNextString(block, token)) {
				return 1;
			}

			static std::unordered_map<std::string, EffectIntermediateTextureFormat> formatMap = {
				{"R8_UNORM", EffectIntermediateTextureFormat::R8_UNORM},
				{"R16_UNORM", EffectIntermediateTextureFormat::R16_UNORM},
				{"R16_FLOAT", EffectIntermediateTextureFormat::R16_FLOAT},
				{"R8G8_UNORM", EffectIntermediateTextureFormat::R8G8_UNORM},
				{"B5G6R5_UNORM", EffectIntermediateTextureFormat::B5G6R5_UNORM},
				{"R16G16_UNORM", EffectIntermediateTextureFormat::R16G16_UNORM},
				{"R16G16_FLOAT", EffectIntermediateTextureFormat::R16G16_FLOAT},
				{"R8G8B8A8_UNORM", EffectIntermediateTextureFormat::R8G8B8A8_UNORM},
				{"B8G8R8A8_UNORM", EffectIntermediateTextureFormat::B8G8R8A8_UNORM},
				{"R10G10B10A2_UNORM", EffectIntermediateTextureFormat::R10G10B10A2_UNORM},
				{"R32_FLOAT", EffectIntermediateTextureFormat::R32_FLOAT},
				{"R11G11B10_FLOAT", EffectIntermediateTextureFormat::R11G11B10_FLOAT},
				{"R32G32_FLOAT", EffectIntermediateTextureFormat::R32G32_FLOAT},
				{"R16G16B16A16_UNORM", EffectIntermediateTextureFormat::R16G16B16A16_UNORM},
				{"R16G16B16A16_FLOAT", EffectIntermediateTextureFormat::R16G16B16A16_FLOAT},
				{"R32G32B32A32_FLOAT", EffectIntermediateTextureFormat::R32G32B32A32_FLOAT}
			};

			auto it = formatMap.find(std::string(token));
			if (it == formatMap.end()) {
				return 1;
			}

			texDesc.format = it->second;
		} else if (t == "WIDTH") {
			if (processed[0] || processed[2]) {
				return 1;
			}
			processed[2] = true;

			if (GetNextExpr(block, texDesc.sizeExpr.first)) {
				return 1;
			}
		} else if (t == "HEIGHT") {
			if (processed[0] || processed[3]) {
				return 1;
			}
			processed[3] = true;

			if (GetNextExpr(block, texDesc.sizeExpr.second)) {
				return 1;
			}
		} else {
			return 1;
		}
	}

	// WIDTH 和 HEIGHT 必须成对出现
	if (processed[2] ^ processed[3]) {
		return 1;
	}

	// 代码部分
	if (!CheckNextToken<true>(block, "Texture2D")) {
		return 1;
	}

	if (GetNextToken<true>(block, token)) {
		return 1;
	}

	if (token == "INPUT") {
		if (processed[1] || processed[2]) {
			return 1;
		}

		// INPUT 已为第一个元素
		desc.textures.pop_back();
	} else {
		// 否则 FORMAT 和 SOURCE 必须二选一
		if (processed[0] == processed[1]) {
			return 1;
		}

		texDesc.name = token;
	}

	if (!CheckNextToken<true>(block, ";")) {
		return 1;
	}

	if (GetNextToken<true>(block, token) != 2) {
		return 1;
	}

	return 0;
}

UINT ResolveSampler(std::string_view block, EffectDesc& desc) {
	// 必选项：FILTER
	// 可选项：ADDRESS

	EffectSamplerDesc& samDesc = desc.samplers.emplace_back();

	std::bitset<2> processed;

	std::string_view token;

	if (!CheckNextToken<true>(block, META_INDICATOR)) {
		return 1;
	}

	if (!CheckNextToken<false>(block, "SAMPLER")) {
		return 1;
	}
	if (GetNextToken<false>(block, token) != 2) {
		return 1;
	}

	while (true) {
		if (!CheckNextToken<true>(block, META_INDICATOR)) {
			break;
		}

		if (GetNextToken<false>(block, token)) {
			return 1;
		}

		std::string t = StrUtils::ToUpperCase(token);

		if (t == "FILTER") {
			if (processed[0]) {
				return 1;
			}
			processed[0] = true;

			if (GetNextString(block, token)) {
				return 1;
			}

			std::string filter = StrUtils::ToUpperCase(token);

			if (filter == "LINEAR") {
				samDesc.filterType = EffectSamplerFilterType::Linear;
			} else if (filter == "POINT") {
				samDesc.filterType = EffectSamplerFilterType::Point;
			} else {
				return 1;
			}
		} else if (t == "ADDRESS") {
			if (processed[1]) {
				return 1;
			}
			processed[1] = true;

			if (GetNextString(block, token)) {
				return 1;
			}

			std::string filter = StrUtils::ToUpperCase(token);

			if (filter == "CLAMP") {
				samDesc.addressType = EffectSamplerAddressType::Clamp;
			} else if (filter == "WRAP") {
				samDesc.addressType = EffectSamplerAddressType::Wrap;
			} else {
				return 1;
			}
		} else {
			return 1;
		}
	}

	if (!processed[0]) {
		return 1;
	}

	// 代码部分
	if (!CheckNextToken<true>(block, "SamplerState")) {
		return 1;
	}

	if (GetNextToken<true>(block, token)) {
		return 1;
	}

	samDesc.name = token;

	if (!CheckNextToken<true>(block, ";")) {
		return 1;
	}

	if (GetNextToken<true>(block, token) != 2) {
		return 1;
	}

	return 0;
}

UINT ResolveCommon(std::string_view& block, std::string& commonHlsl) {
	// 无选项

	if (!CheckNextToken<true>(block, META_INDICATOR)) {
		return 1;
	}

	if (!CheckNextToken<false>(block, "COMMON")) {
		return 1;
	}

	if (CheckNextToken<true>(block, META_INDICATOR)) {
		return 1;
	}

	commonHlsl.append(block);
	if (commonHlsl.back() != '\n') {
		commonHlsl.push_back('\n');
	}

	return 0;
}

UINT ResolvePassNumbers(std::vector<std::string_view>& blocks) {
	// first 为 Pass 序号，second 为在 blocks 中的位置
	std::vector<std::pair<UINT, UINT>> passNumbers;
	passNumbers.reserve(blocks.size());

	std::string_view token;
	for (UINT i = 0; i < blocks.size(); ++i) {
		std::string_view& block = blocks[i];

		if (!CheckNextToken<true>(block, META_INDICATOR)) {
			return 1;
		}

		if (!CheckNextToken<false>(block, "PASS")) {
			return 1;
		}

		UINT index;
		if (GetNextNumber(block, index)) {
			return 1;
		}
		if (GetNextToken<false>(block, token) != 2) {
			return 1;
		}

		passNumbers.emplace_back(index, i);
	}

	std::sort(
		passNumbers.begin(),
		passNumbers.end(),
		[](const std::pair<UINT, UINT>& l, const std::pair<UINT, UINT>& r) {return l.first < r.first; }
	);

	std::vector<std::string_view> temp = blocks;
	for (int i = 0; i < blocks.size(); ++i) {
		if (passNumbers[i].first != i + 1) {
			// PASS 序号不连续
			return 1;
		}

		blocks[i] = temp[passNumbers[i].second];
	}

	return 0;
}

struct TPContext {
	ULONG index;
	std::vector<std::string>& passSources;
	std::vector<EffectPassDesc>& passes;
};

void NTAPI TPWork(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_WORK) {
	TPContext* con = (TPContext*)Context;
	ULONG index = InterlockedIncrement(&con->index);
	
	if (!App::Get().GetDeviceResources().CompileShader(con->passSources[index],
		"__M", con->passes[index].cso.put(), fmt::format("Pass{}", index + 1).c_str(), &passInclude)) {
		con->passes[index].cso = nullptr;
	}
}

UINT ResolvePass(
	std::string_view block,
	EffectDesc& desc,
	UINT passIndex,
	EffectPassDesc& passDesc,
	std::string& passHlsl,
	std::string_view resHlsl,
	std::string_view commonHlsl,
	bool lastEffect,
	bool lastPass
) {
	std::string_view token;

	std::array<UINT, 3> numThreads{};

	// 用于检查输入和输出中重复的纹理
	std::unordered_map<std::string_view, UINT> texNames;
	for (size_t i = 0; i < desc.textures.size(); ++i) {
		texNames.emplace(desc.textures[i].name, (UINT)i);
	}

	std::bitset<4> processed;

	while (true) {
		if (!CheckNextToken<true>(block, META_INDICATOR)) {
			break;
		}

		if (GetNextToken<false>(block, token)) {
			return 1;
		}

		std::string t = StrUtils::ToUpperCase(token);

		if (t == "IN") {
			if (processed[0]) {
				return 1;
			}
			processed[0] = true;

			std::string binds;
			if (GetNextExpr(block, binds)) {
				return 1;
			}

			std::vector<std::string_view> inputs = StrUtils::Split(binds, ',');
			for (const std::string_view& input : inputs) {
				auto it = texNames.find(input);
				if (it == texNames.end()) {
					// 未找到纹理名称
					return 1;
				}

				passDesc.inputs.push_back(it->second);
				texNames.erase(it);
			}
		} else if (t == "OUT") {
			if (processed[1]) {
				return 1;
			}
			processed[1] = true;

			std::string saves;
			if (GetNextExpr(block, saves)) {
				return 1;
			}

			std::vector<std::string_view> outputs = StrUtils::Split(saves, ',');
			if (outputs.size() > 8) {
				// 最多 8 个输出
				return 1;
			}

			for (const std::string_view& output : outputs) {
				auto it = texNames.find(output);
				if (it == texNames.end()) {
					// 未找到纹理名称
					return 1;
				}

				passDesc.outputs.push_back(it->second);
				texNames.erase(it);
			}
		} else if (t == "BLOCK_SIZE") {
			if (processed[2]) {
				return 1;
			}
			processed[2] = true;

			std::string expr;
			if (GetNextExpr(block, expr)) {
				return 1;
			}

			std::vector<std::string_view> blockSize = StrUtils::Split(expr, ',');
			if (blockSize.size() != 2) {
				return 1;
			}
			
			UINT num;
			if (GetNextNumber(blockSize[0], num)) {
				return 1;
			}

			if (GetNextToken<false>(blockSize[0], token) != 2) {
				return false;
			}

			passDesc.blockSize.cx = num;
			
			if (GetNextNumber(blockSize[1], num)) {
				return 1;
			}

			if (GetNextToken<false>(blockSize[1], token) != 2) {
				return false;
			}

			passDesc.blockSize.cy = num;
		} else if (t == "NUM_THREADS") {
			if (processed[3]) {
				return 1;
			}
			processed[3] = true;

			std::string expr;
			if (GetNextExpr(block, expr)) {
				return 1;
			}

			std::vector<std::string_view> split = StrUtils::Split(expr, ',');
			if (split.size() != 3) {
				return 1;
			}

			for (int i = 0; i < 3; ++i) {
				UINT num;
				if (GetNextNumber(split[i], num)) {
					return 1;
				}

				if (GetNextToken<false>(split[i], token) != 2) {
					return false;
				}

				numThreads[i] = num;
			}
		} else {
			return 1;
		}
	}

	// SRV
	for (int i = 0; i < passDesc.inputs.size(); ++i) {
		passHlsl.append(fmt::format("Texture2D {}:register(t{});", desc.textures[passDesc.inputs[i]].name, i));
	}

	if (lastEffect && lastPass) {
		passHlsl.append(fmt::format("Texture2D __CURSOR:register(t{});", passDesc.inputs.size()));
	}

	// UAV
	if (passDesc.outputs.empty()) {
		if (!lastPass) {
			return 1;
		}

		passHlsl.append("RWTexture2D<float4> __OUTPUT:register(u0);");
	} else {
		if (lastPass) {
			return 1;
		}

		for (int i = 0; i < passDesc.outputs.size(); ++i) {
			passHlsl.append(fmt::format("RWTexture2D<float4> {}:register(u{});", desc.textures[passDesc.outputs[i]].name, i));
		}
	}

	passHlsl.append(resHlsl);

	if (lastPass) {
		if (lastEffect) {
			passHlsl.append("void __WriteToOutput(uint2 pos,float3 color) {pos+=__off.zw;if((int)pos.x>=__cr.x &&(int)pos.y>=__cr.y&&(int)pos.x<__cr.z&&(int)pos.y<__cr.w){float4 mask=__CURSOR.SampleLevel(sam,(pos-__cr.xy+0.5f)*__cp,0);if(__ct==0){color=color*mask.a+mask.rgb;}else if(__ct==1){if(mask.a<0.5f){color=mask.rgb;}else{color=(uint3(round(color*255))^uint3(mask.rgb*255))/255.0f;}}else{if(mask.x>0.5f){if(mask.y>0.5f){color=1-color;}}else{if(mask.y>0.5f){color=float3(1,1,1);}else{color=float3(0,0,0);}}}}__OUTPUT[pos]=float4(color,1);}\n#define WriteToOutput(pos,color) if(pos.x<__vx&&pos.y<__vy)__WriteToOutput(pos,color)\n");
		} else {
			passHlsl.append("#define WriteToOutput(pos,color) __OUTPUT[pos]=float4(color, 1)\n");
		}
	}

	passHlsl.append(commonHlsl).append(block);

	if (passHlsl.back() != '\n') {
		passHlsl.push_back('\n');
	}

	// main 函数
	/*if (passDesc.outputs.size() <= 1) {
		passHlsl.append(fmt::format("float4 __M(float4 p:SV_POSITION,float2 c:TEXCOORD):SV_TARGET"
			"{{return Pass{}(c);}}", passIndex));
	} else {
		// 多渲染目标
		passHlsl.append("void __M(float4 p:SV_POSITION,float2 c:TEXCOORD,out float4 t0:SV_TARGET0,out float4 t1:SV_TARGET1");
		for (int i = 2; i < passDesc.outputs.size(); ++i) {
			passHlsl.append(fmt::format(",out float4 t{0}:SV_TARGET{0}", i));
		}
		passHlsl.append(fmt::format("){{Pass{}(c,t0,t1", passIndex));
		for (int i = 2; i < passDesc.outputs.size(); ++i) {
			passHlsl.append(fmt::format(",t{}", i));
		}
		passHlsl.append(");}");
	}*/

	passHlsl.append(fmt::format("[numthreads({},{},{})]\nvoid __M(uint3 tid:SV_GroupThreadID,uint3 gid:SV_GroupID){{Pass{}(gid.xy*uint2({},{}){},tid);}}", numThreads[0], numThreads[1], numThreads[2], passIndex, passDesc.blockSize.cx, passDesc.blockSize.cy, lastEffect&& lastPass ? "+__off.xy" : ""));

	return 0;
}

UINT ResolvePasses(std::vector<std::string_view>& blocks, std::string_view commonHlsl, EffectDesc& desc, UINT flags) {
	// 可选项：BIND，SAVE

	std::string resHlsl;

	bool lastEffect = flags & EffectCompiler::COMPILE_FLAG_LAST_EFFECT;

	if (!desc.constants.empty() || !desc.valueConstants.empty() || lastEffect) {
		// 常量缓冲区
		resHlsl.append("cbuffer __C:register(b0){");

		if (lastEffect) {
			resHlsl.append("uint4 __off;uint __vx;uint __vy;");
		}

		for (const auto& d : desc.constants) {
			resHlsl.append(d.type == EffectConstantType::Int ? "int " : "float ")
				.append(d.name)
				.append(";");
		}

		for (const auto& d : desc.valueConstants) {
			resHlsl.append(d.type == EffectConstantType::Int ? "int " : "float ")
				.append(d.name)
				.append(";");
		}

		resHlsl.append("};");
	}
	
	if (!desc.dynamicValueConstants.empty() || lastEffect) {
		// 每帧更新的常量
		resHlsl.append("cbuffer __D:register(b1){");

		if (lastEffect) {
			resHlsl.append("int4 __cr;float2 __cp;uint __ct;");
		}

		for (const auto& d : desc.dynamicValueConstants) {
			resHlsl.append(d.type == EffectConstantType::Int ? "int " : "float ")
				.append(d.name)
				.append(";");
		}

		resHlsl.append("};");
	}
	if (!desc.samplers.empty()) {
		// 采样器
		for (int i = 0; i < desc.samplers.size(); ++i) {
			resHlsl.append(fmt::format("SamplerState {}:register(s{});", desc.samplers[i].name, i));
		}
	}
	resHlsl.push_back('\n');

	if (ResolvePassNumbers(blocks)) {
		Logger::Get().Error("解析 Pass 序号失败");
		return 1;
	}

	std::vector<std::string> passSources(blocks.size());
	desc.passes.resize(blocks.size());

	for (UINT i = 0; i < blocks.size(); ++i) {
		if (ResolvePass(
			blocks[i], desc, i + 1, desc.passes[i], passSources[i],
			resHlsl, commonHlsl, lastEffect, i == blocks.size() - 1)
		) {
			Logger::Get().Error(fmt::format("解析 Pass{} 失败", i + 1));
			return 1;
		}
	}

	// 编译生成的 hlsl
	assert(!passSources.empty());
	auto& dr = App::Get().GetDeviceResources();

	if (passSources.size() == 1) {
		if (!dr.CompileShader(passSources[0], "__M", desc.passes[0].cso.put(), "Pass1", &passInclude)) {
			Logger::Get().Error("编译 Pass1 失败");
			return 1;
		}
	} else {
		// 有多个 Pass，使用线程池加速编译
		TPContext context = {
			0,
			passSources,
			desc.passes
		};

		PTP_WORK work = CreateThreadpoolWork(TPWork, &context, nullptr);

		if (work) {
			for (size_t i = 1; i < passSources.size(); ++i) {
				SubmitThreadpoolWork(work);
			}

			dr.CompileShader(passSources[0], "__M", desc.passes[0].cso.put(), "Pass1", &passInclude);

			WaitForThreadpoolWorkCallbacks(work, FALSE);
			CloseThreadpoolWork(work);

			for (size_t i = 0; i < passSources.size(); ++i) {
				if (!desc.passes[i].cso) {
					Logger::Get().Error(fmt::format("编译 Pass{} 失败", i + 1));
					return 1;
				}
			}
		} else {
			Logger::Get().Win32Error("CreateThreadpoolWork 失败，回退到单线程编译");

			// 回退到单线程
			for (size_t i = 0; i < passSources.size(); ++i) {
				if (!dr.CompileShader(passSources[i], "__M", desc.passes[i].cso.put(), fmt::format("Pass{}", i + 1).c_str(), &passInclude)) {
					Logger::Get().Error(fmt::format("编译 Pass{} 失败", i + 1));
					return 1;
				}
			}
		}
	}

	return 0;
}


UINT EffectCompiler::Compile(const wchar_t* fileName, EffectDesc& desc, UINT flags) {
	desc = {};

	std::string source;
	if (!Utils::ReadTextFile(fileName, source)) {
		Logger::Get().Error("读取源文件失败");
		return 1;
	}

	if (source.empty()) {
		Logger::Get().Error("源文件为空");
		return 1;
	}

	// 移除注释
	if (RemoveComments(source)) {
		Logger::Get().Error("删除注释失败");
		return 1;
	}

	std::string md5;
	if (!App::Get().IsDisableEffectCache()) {
		std::vector<BYTE> hash;
		if (!Utils::Hasher::Get().Hash(source.data(), source.size(), hash)) {
			Logger::Get().Error("计算 hash 失败");
		} else {
			md5 = Utils::Bin2Hex(hash.data(), hash.size());

			if (EffectCache::Get().Load(fileName, md5, desc)) {
				// 已从缓存中读取
				return 0;
			}
		}
	}

	std::string_view sourceView(source);

	// 检查头
	if (!CheckMagic(sourceView)) {
		Logger::Get().Error("检查 MagpieFX 头失败");
		return 2;
	}

	enum class BlockType {
		Header,
		Constant,
		Texture,
		Sampler,
		Common,
		Pass
	};

	std::string_view headerBlock;
	std::vector<std::string_view> constantBlocks;
	std::vector<std::string_view> textureBlocks;
	std::vector<std::string_view> samplerBlocks;
	std::vector<std::string_view> commonBlocks;
	std::vector<std::string_view> passBlocks;

	BlockType curBlockType = BlockType::Header;
	size_t curBlockOff = 0;

	auto completeCurrentBlock = [&](size_t len, BlockType newBlockType) {
		switch (curBlockType) {
		case BlockType::Header:
			headerBlock = sourceView.substr(curBlockOff, len);
			break;
		case BlockType::Constant:
			constantBlocks.push_back(sourceView.substr(curBlockOff, len));
			break;
		case BlockType::Texture:
			textureBlocks.push_back(sourceView.substr(curBlockOff, len));
			break;
		case BlockType::Sampler:
			samplerBlocks.push_back(sourceView.substr(curBlockOff, len));
			break;
		case BlockType::Common:
			commonBlocks.push_back(sourceView.substr(curBlockOff, len));
			break;
		case BlockType::Pass:
			passBlocks.push_back(sourceView.substr(curBlockOff, len));
			break;
		default:
			assert(false);
			break;
		}

		curBlockType = newBlockType;
		curBlockOff += len;
	};

	bool newLine = true;
	std::string_view t = sourceView;
	while (t.size() > 5) {
		if (newLine) {
			// 包含换行符
			size_t len = t.data() - sourceView.data() - curBlockOff + 1;

			if (CheckNextToken<true>(t, META_INDICATOR)) {
				std::string_view token;
				if (GetNextToken<false>(t, token)) {
					return 1;
				}
				std::string blockType = StrUtils::ToUpperCase(token);

				if (blockType == "CONSTANT") {
					completeCurrentBlock(len, BlockType::Constant);
				} else if (blockType == "TEXTURE") {
					completeCurrentBlock(len, BlockType::Texture);
				} else if (blockType == "SAMPLER") {
					completeCurrentBlock(len, BlockType::Sampler);
				} else if (blockType == "COMMON") {
					completeCurrentBlock(len, BlockType::Common);
				} else if (blockType == "PASS") {
					completeCurrentBlock(len, BlockType::Pass);
				}
			}

			if (t.size() <= 5) {
				break;
			}
		} else {
			t.remove_prefix(1);
		}

		newLine = t[0] == '\n';
	}

	completeCurrentBlock(sourceView.size() - curBlockOff, BlockType::Header);

	// 必须有 PASS 块
	if (passBlocks.empty()) {
		Logger::Get().Error("无 PASS 块");
		return 1;
	}

	if (ResolveHeader(headerBlock, desc)) {
		Logger::Get().Error("解析 Header 块失败");
		return 1;
	}

	for (size_t i = 0; i < constantBlocks.size(); ++i) {
		if (ResolveConstant(constantBlocks[i], desc)) {
			Logger::Get().Error(fmt::format("解析 Constant#{} 块失败", i + 1));
			return 1;
		}
	}

	// 纹理第一个元素为 INPUT
	EffectIntermediateTextureDesc& inputTex = desc.textures.emplace_back();
	inputTex.name = "INPUT";
	for (size_t i = 0; i < textureBlocks.size(); ++i) {
		if (ResolveTexture(textureBlocks[i], desc)) {
			Logger::Get().Error(fmt::format("解析 Texture#{} 块失败", i + 1));
			return 1;
		}
	}

	for (size_t i = 0; i < samplerBlocks.size(); ++i) {
		if (ResolveSampler(samplerBlocks[i], desc)) {
			Logger::Get().Error(fmt::format("解析 Sampler#{} 块失败", i + 1));
			return 1;
		}
	}

	{
		// 确保没有重复的名字
		std::unordered_set<std::string> names;
		for (const auto& d : desc.constants) {
			if (names.find(d.name) != names.end()) {
				Logger::Get().Error("标识符重复");
				return 1;
			}
			names.insert(d.name);
		}
		for (const auto& d : desc.textures) {
			if (names.find(d.name) != names.end()) {
				Logger::Get().Error("标识符重复");
				return 1;
			}
			names.insert(d.name);
		}
		for (const auto& d : desc.samplers) {
			if (names.find(d.name) != names.end()) {
				Logger::Get().Error("标识符重复");
				return 1;
			}
			names.insert(d.name);
		}
	}

	std::string commonHlsl;

	for (size_t i = 0; i < commonBlocks.size(); ++i) {
		if (ResolveCommon(commonBlocks[i], commonHlsl)) {
			Logger::Get().Error(fmt::format("解析 Common#{} 块失败", i + 1));
			return 1;
		}
	}

	if (ResolvePasses(passBlocks, commonHlsl, desc, flags)) {
		Logger::Get().Error("解析 Pass 块失败");
		return 1;
	}

	EffectCache::Get().Save(fileName, md5, desc);

	return 0;
}
