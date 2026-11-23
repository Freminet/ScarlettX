#include "scarlettx/gpu/shader_ir.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <d3dcompiler.h>
#endif

namespace scarlettx {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void WriteU32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

constexpr uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

constexpr uint32_t kMd5Shift[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

void Md5Transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f = 0;
        uint32_t g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const uint32_t x = a + f + kMd5K[i] + ReadU32(block + g * 4);
        const uint32_t s = kMd5Shift[(i / 16) * 4 + (i % 4)];
        a = d;
        d = c;
        c = b;
        b += (x << s) | (x >> (32 - s));
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

// The checksum in bytes 4..19 of a DXBC/DXIL container: MD5 over everything after it, with
// the D3D compiler's own padding (bit count in front of the last block, (bits >> 2) | 1 at
// its end). D3D12 refuses a container whose checksum does not match.
void WriteDxbcChecksum(std::vector<uint8_t>& container) {
    const uint8_t* data = container.data() + 20;
    const size_t size = container.size() - 20;
    const uint32_t bits = static_cast<uint32_t>(size * 8);
    const size_t full = size & ~size_t{63};
    const size_t rest = size - full;

    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t off = 0; off < full; off += 64) {
        Md5Transform(state, data + off);
    }

    uint8_t block[64] = {};
    if (rest >= 56) {
        std::memcpy(block, data + full, rest);
        block[rest] = 0x80;
        Md5Transform(state, block);
        std::memset(block, 0, sizeof(block));
        WriteU32(block, bits);
    } else {
        WriteU32(block, bits);
        std::memcpy(block + 4, data + full, rest);
        block[4 + rest] = 0x80;
    }
    WriteU32(block + 60, (bits >> 2) | 1);
    Md5Transform(state, block);

    for (int i = 0; i < 4; ++i) {
        WriteU32(container.data() + 4 + i * 4, state[i]);
    }
}

bool IsXboxPrivateFourCC(const std::string& fourcc) {
    return fourcc == "XBOX" || fourcc == "XS2M" || fourcc == "XDBG" || fourcc == "XBIN";
}

ShaderStage DecodeDxbcProgramType(uint16_t program_type) {
    switch (program_type) {
        case 0: return ShaderStage::Pixel;
        case 1: return ShaderStage::Vertex;
        case 2: return ShaderStage::Geometry;
        case 3: return ShaderStage::Hull;
        case 4: return ShaderStage::Domain;
        case 5: return ShaderStage::Compute;
        case 6: return ShaderStage::Library;
        default: return ShaderStage::Unknown;
    }
}

}  // namespace

uint64_t HashShaderBytes(const uint8_t* data, size_t size) {
    uint64_t h = kFnvOffset;
    if (!data) {
        return h;
    }
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnvPrime;
    }
    return h;
}

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info) {
    out_info = ShaderModuleInfo{};
    out_info.byte_size = static_cast<uint32_t>(bytecode.size());
    out_info.content_hash = HashShaderBytes(bytecode.data(), bytecode.size());

    if (bytecode.size() >= 4 && std::memcmp(bytecode.data(), "XS2\0", 4) == 0) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
        return Status::Ok();
    }

    if (bytecode.size() < 32) {
        return Status::Error(StatusCode::ShaderError, "Shader container too small (< 32 bytes)");
    }

    if (std::memcmp(bytecode.data(), "DXBC", 4) == 0) {
        const uint32_t chunk_count = ReadU32(bytecode.data() + 28);
        out_info.chunk_count = chunk_count;
        out_info.kind = ShaderContainerKind::Dxbc;

        const size_t offsets_pos = 32;
        if (offsets_pos + static_cast<size_t>(chunk_count) * 4 > bytecode.size()) {
            return Status::Error(StatusCode::ShaderError, "DXBC chunk offset table truncated");
        }

        for (uint32_t i = 0; i < chunk_count; ++i) {
            const uint32_t chunk_off = ReadU32(bytecode.data() + offsets_pos + i * 4);
            if (static_cast<size_t>(chunk_off) + 8 > bytecode.size()) {
                continue;
            }
            std::string fourcc(reinterpret_cast<const char*>(bytecode.data() + chunk_off), 4);
            const uint32_t chunk_size = ReadU32(bytecode.data() + chunk_off + 4);
            const size_t data_off = static_cast<size_t>(chunk_off) + 8;

            if (fourcc == "DXIL" || fourcc == "ILDB") {
                out_info.kind = ShaderContainerKind::Dxil;
                if (data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                    const uint32_t ver_token = ReadU32(bytecode.data() + data_off);
                    const uint16_t prog_type = static_cast<uint16_t>((ver_token >> 16) & 0xFFFFu);
                    out_info.stage = DecodeDxbcProgramType(prog_type);
                    out_info.shader_model_major = (ver_token >> 4) & 0xFu;
                    out_info.shader_model_minor = ver_token & 0xFu;
                }
            } else if ((fourcc == "SHEX" || fourcc == "SHDR") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                const uint32_t ver_token = ReadU32(bytecode.data() + data_off);
                const uint16_t prog_type = static_cast<uint16_t>((ver_token >> 16) & 0xFFFFu);
                out_info.stage = DecodeDxbcProgramType(prog_type);
                out_info.shader_model_major = (ver_token >> 4) & 0xFu;
                out_info.shader_model_minor = ver_token & 0xFu;
            } else if ((fourcc == "ISGN" || fourcc == "ISG1") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                out_info.input_param_count = ReadU32(bytecode.data() + data_off);
            } else if ((fourcc == "OSGN" || fourcc == "OSG1") &&
                       data_off + 4 <= bytecode.size() && chunk_size >= 4) {
                out_info.output_param_count = ReadU32(bytecode.data() + data_off);
            } else if (fourcc == "RDAT") {
                out_info.has_raytracing_exports = true;
                if (out_info.stage == ShaderStage::Unknown) {
                    out_info.stage = ShaderStage::Library;
                }
            } else if (fourcc == "RTS0") {
                out_info.has_embedded_root_signature = true;
            }

            if (IsXboxPrivateFourCC(fourcc)) {
                out_info.has_xbox_private_chunks = true;
            }

            out_info.chunk_fourccs.push_back(std::move(fourcc));
        }
        return Status::Ok();
    }

    if (std::memcmp(bytecode.data(), "XS2\0", 4) == 0) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
        return Status::Ok();
    }

    return Status::Error(StatusCode::UnsupportedFormat, "Unrecognized shader container magic");
}

Status TranslateXboxShaderToPc(
    const std::vector<uint8_t>& xbox_bytecode,
    ShaderTranslationResult& out_result) {
    out_result = ShaderTranslationResult{};
    Status st = InspectShaderContainer(xbox_bytecode, out_result.source_info);
    if (!st.ok()) {
        return st;
    }

    if (out_result.source_info.kind == ShaderContainerKind::ScarlettRdna2) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Precompiled Scarlett RDNA2 (XS2) shader lifting to DXIL is not yet implemented");
    }

    if (!out_result.source_info.has_xbox_private_chunks) {
        out_result.pc_bytecode = xbox_bytecode;
        out_result.translated_hash = out_result.source_info.content_hash;
        out_result.stripped_xbox_chunks = 0;
        return Status::Ok();
    }

    // Rebuild DXBC container without Xbox-private chunks while preserving all standard chunks.
    const uint32_t orig_count = ReadU32(xbox_bytecode.data() + 28);
    struct ChunkSlice {
        const uint8_t* ptr;
        size_t total_len;
    };
    std::vector<ChunkSlice> kept;
    kept.reserve(orig_count);

    for (uint32_t i = 0; i < orig_count; ++i) {
        const uint32_t off = ReadU32(xbox_bytecode.data() + 32 + i * 4);
        if (static_cast<size_t>(off) + 8 > xbox_bytecode.size()) {
            continue;
        }
        std::string fourcc(reinterpret_cast<const char*>(xbox_bytecode.data() + off), 4);
        const uint32_t sz = ReadU32(xbox_bytecode.data() + off + 4);
        const size_t total_len = 8ull + static_cast<size_t>(sz);
        if (static_cast<size_t>(off) + total_len > xbox_bytecode.size()) {
            return Status::Error(StatusCode::ShaderError, "DXBC chunk extends past end of buffer");
        }
        if (IsXboxPrivateFourCC(fourcc)) {
            ++out_result.stripped_xbox_chunks;
            continue;
        }
        kept.push_back({xbox_bytecode.data() + off, total_len});
    }

    const size_t header_and_table = 32ull + kept.size() * 4ull;
    size_t total_size = header_and_table;
    for (const auto& c : kept) {
        total_size += c.total_len;
    }

    out_result.pc_bytecode.resize(total_size, 0);
    // Copy DXBC magic + version fields (bytes 0..23), then write updated size + chunk count.
    std::memcpy(out_result.pc_bytecode.data(), xbox_bytecode.data(), 24);
    WriteU32(out_result.pc_bytecode.data() + 24, static_cast<uint32_t>(total_size));
    WriteU32(out_result.pc_bytecode.data() + 28, static_cast<uint32_t>(kept.size()));

    size_t cursor = header_and_table;
    for (size_t i = 0; i < kept.size(); ++i) {
        WriteU32(out_result.pc_bytecode.data() + 32 + i * 4, static_cast<uint32_t>(cursor));
        std::memcpy(out_result.pc_bytecode.data() + cursor, kept[i].ptr, kept[i].total_len);
        cursor += kept[i].total_len;
    }
    WriteDxbcChecksum(out_result.pc_bytecode);

    out_result.translated_hash =
        HashShaderBytes(out_result.pc_bytecode.data(), out_result.pc_bytecode.size());
    return Status::Ok();
}

Status CompileHlslToDxbc(
    const std::string& hlsl_source,
    const std::string& entry_point,
    const std::string& target,
    std::vector<uint8_t>& out_bytecode) {
    out_bytecode.clear();
#if defined(_WIN32)
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        hlsl_source.data(),
        hlsl_source.size(),
        "scarlettx_inline.hlsl",
        nullptr,
        nullptr,
        entry_point.c_str(),
        target.c_str(),
        D3DCOMPILE_OPTIMIZATION_LEVEL1,
        0,
        &code,
        &errors);
    if (FAILED(hr) || !code) {
        std::string msg = "D3DCompile failed";
        if (errors && errors->GetBufferPointer() && errors->GetBufferSize() > 0) {
            msg += ": ";
            msg.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        if (errors) errors->Release();
        if (code) code->Release();
        return Status::Error(StatusCode::ShaderError, msg);
    }
    const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
    out_bytecode.assign(bytes, bytes + code->GetBufferSize());
    code->Release();
    if (errors) errors->Release();
    return Status::Ok();
#else
    (void)hlsl_source;
    (void)entry_point;
    (void)target;
    return Status::Error(StatusCode::NotImplemented, "D3DCompile requires Windows");
#endif
}

bool ShaderCache::Lookup(uint64_t key, std::vector<uint8_t>& out_blob) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++misses_;
        return false;
    }
    ++hits_;
    out_blob = it->second;
    return true;
}

void ShaderCache::Store(uint64_t key, std::vector<uint8_t> blob) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = std::move(blob);
}

void ShaderCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    hits_ = 0;
    misses_ = 0;
}

uint64_t ShaderCache::hits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

uint64_t ShaderCache::misses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
}

size_t ShaderCache::entry_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace scarlettx
