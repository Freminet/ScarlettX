#include "scarlettx/gpu/rdna2_xs2.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "scarlettx/gpu/shader_ir.h"

namespace scarlettx::gpu {
namespace {

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void WriteU16(std::vector<uint8_t>& buf, uint16_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}

void WriteU32(std::vector<uint8_t>& buf, uint32_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}

float DecodeInlineFloat(uint16_t code) {
    switch (code) {
        case 240: return 0.5f;
        case 241: return -0.5f;
        case 242: return 1.0f;
        case 243: return -1.0f;
        case 244: return 2.0f;
        case 245: return -2.0f;
        case 246: return 4.0f;
        case 247: return -4.0f;
        case 248: return 0.15915494f;  // 1 / (2 * pi)
        default: return 0.0f;
    }
}

uint32_t FloatToBits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

Rdna2Operand DecodeSrcOperand(uint16_t code, bool allow_vgpr, uint32_t literal_u32) {
    Rdna2Operand op{};
    if (allow_vgpr && code >= 256 && code <= 511) {
        op.kind = Rdna2OperandKind::Vgpr;
        op.reg_index = static_cast<uint16_t>(code - 256);
        return op;
    }
    if (code <= 105) {
        op.kind = Rdna2OperandKind::Sgpr;
        op.reg_index = code;
        return op;
    }
    if (code == 106) { op.kind = Rdna2OperandKind::VccLo; return op; }
    if (code == 107) { op.kind = Rdna2OperandKind::VccHi; return op; }
    if (code == 124) { op.kind = Rdna2OperandKind::M0; return op; }
    if (code == 125) { op.kind = Rdna2OperandKind::Null; return op; }
    if (code == 126) { op.kind = Rdna2OperandKind::ExecLo; return op; }
    if (code == 127) { op.kind = Rdna2OperandKind::ExecHi; return op; }
    if (code >= 128 && code <= 192) {
        op.kind = Rdna2OperandKind::InlineInt;
        op.imm_i32 = static_cast<int32_t>(code - 128);
        return op;
    }
    if (code >= 193 && code <= 208) {
        op.kind = Rdna2OperandKind::InlineInt;
        op.imm_i32 = 192 - static_cast<int32_t>(code);
        return op;
    }
    if (code >= 240 && code <= 248) {
        op.kind = Rdna2OperandKind::InlineFloat;
        op.imm_f32 = DecodeInlineFloat(code);
        return op;
    }
    if (code == 253) { op.kind = Rdna2OperandKind::Scc; return op; }
    if (code == 255) {
        op.kind = Rdna2OperandKind::Literal32;
        op.literal_u32 = literal_u32;
        return op;
    }
    op.kind = Rdna2OperandKind::InlineInt;
    op.imm_i32 = 0;
    return op;
}

Rdna2Operand DecodeScalarDst(uint8_t code) {
    if (code <= 105) return Rdna2Operand::Sgpr(code);
    Rdna2Operand op{};
    if (code == 106) op.kind = Rdna2OperandKind::VccLo;
    else if (code == 107) op.kind = Rdna2OperandKind::VccHi;
    else if (code == 124) op.kind = Rdna2OperandKind::M0;
    else if (code == 125) op.kind = Rdna2OperandKind::Null;
    else if (code == 126) op.kind = Rdna2OperandKind::ExecLo;
    else if (code == 127) op.kind = Rdna2OperandKind::ExecHi;
    else op.kind = Rdna2OperandKind::Null;
    return op;
}

Rdna2Mnemonic MapSop2Opcode(uint16_t op) {
    switch (op) {
        case 0x00: return Rdna2Mnemonic::S_ADD_U32;
        case 0x01: return Rdna2Mnemonic::S_SUB_U32;
        case 0x02: return Rdna2Mnemonic::S_ADD_I32;
        case 0x03: return Rdna2Mnemonic::S_SUB_I32;
        case 0x06: return Rdna2Mnemonic::S_MIN_I32;
        case 0x07: return Rdna2Mnemonic::S_MIN_U32;
        case 0x08: return Rdna2Mnemonic::S_MAX_I32;
        case 0x09: return Rdna2Mnemonic::S_MAX_U32;
        case 0x0C: return Rdna2Mnemonic::S_AND_B32;
        case 0x0E: return Rdna2Mnemonic::S_OR_B32;
        case 0x10: return Rdna2Mnemonic::S_XOR_B32;
        case 0x1C: return Rdna2Mnemonic::S_LSHL_B32;
        case 0x1E: return Rdna2Mnemonic::S_LSHR_B32;
        case 0x20: return Rdna2Mnemonic::S_ASHR_I32;
        case 0x24: return Rdna2Mnemonic::S_MUL_I32;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapSop1Opcode(uint16_t op) {
    switch (op) {
        case 0x03: return Rdna2Mnemonic::S_MOV_B32;
        case 0x04: return Rdna2Mnemonic::S_MOV_B64;
        case 0x07: return Rdna2Mnemonic::S_NOT_B32;
        case 0x1C: return Rdna2Mnemonic::S_WQM_B32;
        case 0x1F: return Rdna2Mnemonic::S_GETPC_B64;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapSopkOpcode(uint16_t op) {
    switch (op) {
        case 0x00: return Rdna2Mnemonic::S_MOVK_I32;
        case 0x02: return Rdna2Mnemonic::S_CMOVK_I32;
        case 0x03: return Rdna2Mnemonic::S_CMPK_EQ_I32;
        case 0x04: return Rdna2Mnemonic::S_CMPK_LG_I32;
        case 0x05: return Rdna2Mnemonic::S_CMPK_GT_I32;
        case 0x06: return Rdna2Mnemonic::S_CMPK_GE_I32;
        case 0x07: return Rdna2Mnemonic::S_CMPK_LT_I32;
        case 0x08: return Rdna2Mnemonic::S_CMPK_LE_I32;
        case 0x0F: return Rdna2Mnemonic::S_ADDK_I32;
        case 0x10: return Rdna2Mnemonic::S_MULK_I32;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapSopcOpcode(uint16_t op) {
    switch (op) {
        case 0x00: return Rdna2Mnemonic::S_CMP_EQ_I32;
        case 0x01: return Rdna2Mnemonic::S_CMP_LG_I32;
        case 0x02: return Rdna2Mnemonic::S_CMP_GT_I32;
        case 0x03: return Rdna2Mnemonic::S_CMP_GE_I32;
        case 0x04: return Rdna2Mnemonic::S_CMP_LT_I32;
        case 0x05: return Rdna2Mnemonic::S_CMP_LE_I32;
        case 0x06: return Rdna2Mnemonic::S_CMP_EQ_U32;
        case 0x07: return Rdna2Mnemonic::S_CMP_LG_U32;
        case 0x08: return Rdna2Mnemonic::S_CMP_GT_U32;
        case 0x09: return Rdna2Mnemonic::S_CMP_GE_U32;
        case 0x0A: return Rdna2Mnemonic::S_CMP_LT_U32;
        case 0x0B: return Rdna2Mnemonic::S_CMP_LE_U32;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapSoppOpcode(uint16_t op) {
    switch (op) {
        case 0x00: return Rdna2Mnemonic::S_NOP;
        case 0x01: return Rdna2Mnemonic::S_ENDPGM;
        case 0x02: return Rdna2Mnemonic::S_BRANCH;
        case 0x04: return Rdna2Mnemonic::S_CBRANCH_SCC0;
        case 0x05: return Rdna2Mnemonic::S_CBRANCH_SCC1;
        case 0x06: return Rdna2Mnemonic::S_CBRANCH_VCCZ;
        case 0x07: return Rdna2Mnemonic::S_CBRANCH_VCCNZ;
        case 0x08: return Rdna2Mnemonic::S_CBRANCH_EXECZ;
        case 0x09: return Rdna2Mnemonic::S_CBRANCH_EXECNZ;
        case 0x0A: return Rdna2Mnemonic::S_BARRIER;
        case 0x0C: return Rdna2Mnemonic::S_WAITCNT;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapVop2Opcode(uint16_t op) {
    switch (op) {
        case 0x01: return Rdna2Mnemonic::V_CNDMASK_B32;
        case 0x03: return Rdna2Mnemonic::V_ADD_F32;
        case 0x04: return Rdna2Mnemonic::V_SUB_F32;
        case 0x08: return Rdna2Mnemonic::V_MUL_F32;
        case 0x0F: return Rdna2Mnemonic::V_MIN_F32;
        case 0x10: return Rdna2Mnemonic::V_MAX_F32;
        case 0x18: return Rdna2Mnemonic::V_LSHLREV_B32;
        case 0x19: return Rdna2Mnemonic::V_LSHRREV_B32;
        case 0x1B: return Rdna2Mnemonic::V_AND_B32;
        case 0x1C: return Rdna2Mnemonic::V_OR_B32;
        case 0x1D: return Rdna2Mnemonic::V_XOR_B32;
        case 0x25: return Rdna2Mnemonic::V_ADD_NC_U32;
        case 0x26: return Rdna2Mnemonic::V_SUB_NC_U32;
        case 0x3B: return Rdna2Mnemonic::V_FMAC_F32;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapVop1Opcode(uint16_t op) {
    switch (op) {
        case 0x00: return Rdna2Mnemonic::V_NOP;
        case 0x01: return Rdna2Mnemonic::V_MOV_B32;
        case 0x05: return Rdna2Mnemonic::V_CVT_F32_I32;
        case 0x06: return Rdna2Mnemonic::V_CVT_F32_U32;
        case 0x08: return Rdna2Mnemonic::V_CVT_I32_F32;
        case 0x1B: return Rdna2Mnemonic::V_FRACT_F32;
        case 0x24: return Rdna2Mnemonic::V_FLOOR_F32;
        case 0x25: return Rdna2Mnemonic::V_RSQ_F32;
        case 0x27: return Rdna2Mnemonic::V_SQRT_F32;
        case 0x2A: return Rdna2Mnemonic::V_RCP_F32;
        case 0x35: return Rdna2Mnemonic::V_SIN_F32;
        case 0x36: return Rdna2Mnemonic::V_COS_F32;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapVopcOpcode(uint16_t op) {
    switch (op) {
        case 0x01: return Rdna2Mnemonic::V_CMP_LT_F32;
        case 0x02: return Rdna2Mnemonic::V_CMP_EQ_F32;
        case 0x03: return Rdna2Mnemonic::V_CMP_LE_F32;
        case 0x04: return Rdna2Mnemonic::V_CMP_GT_F32;
        case 0x05: return Rdna2Mnemonic::V_CMP_NEQ_F32;
        case 0x06: return Rdna2Mnemonic::V_CMP_GE_F32;
        case 0xC1: return Rdna2Mnemonic::V_CMP_LT_U32;
        case 0xC2: return Rdna2Mnemonic::V_CMP_EQ_U32;
        case 0xC4: return Rdna2Mnemonic::V_CMP_GT_U32;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapVop3Opcode(uint16_t op) {
    if (op < 0x100u) {
        return MapVopcOpcode(op);
    }
    if (op >= 0x100u && op < 0x140u) {
        return MapVop2Opcode(static_cast<uint16_t>(op - 0x100u));
    }
    if (op >= 0x140u && op < 0x1C0u) {
        return MapVop1Opcode(static_cast<uint16_t>(op - 0x140u));
    }
    switch (op) {
        case 0x1C1: return Rdna2Mnemonic::V_MAD_F32;
        case 0x1C3: return Rdna2Mnemonic::V_MAD_U32_U24;
        case 0x1C8: return Rdna2Mnemonic::V_BFE_U32;
        case 0x1CA: return Rdna2Mnemonic::V_BFI_B32;
        case 0x1CB: return Rdna2Mnemonic::V_FMA_F32;
        case 0x1ED: return Rdna2Mnemonic::V_PERM_B32;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapSmemOpcode(uint16_t op) {
    switch (op) {
        case 0x00: return Rdna2Mnemonic::S_LOAD_DWORD;
        case 0x01: return Rdna2Mnemonic::S_LOAD_DWORDX2;
        case 0x02: return Rdna2Mnemonic::S_LOAD_DWORDX4;
        case 0x08: return Rdna2Mnemonic::S_BUFFER_LOAD_DWORD;
        case 0x09: return Rdna2Mnemonic::S_BUFFER_LOAD_DWORDX2;
        case 0x0A: return Rdna2Mnemonic::S_BUFFER_LOAD_DWORDX4;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapMubufOpcode(uint16_t op) {
    switch (op) {
        case 0x0C: return Rdna2Mnemonic::BUFFER_LOAD_DWORD;
        case 0x0D: return Rdna2Mnemonic::BUFFER_LOAD_DWORDX2;
        case 0x0E: return Rdna2Mnemonic::BUFFER_LOAD_DWORDX4;
        case 0x1C: return Rdna2Mnemonic::BUFFER_STORE_DWORD;
        case 0x1D: return Rdna2Mnemonic::BUFFER_STORE_DWORDX2;
        case 0x1E: return Rdna2Mnemonic::BUFFER_STORE_DWORDX4;
        default: return Rdna2Mnemonic::Unknown;
    }
}

Rdna2Mnemonic MapFlatOpcode(uint16_t op) {
    switch (op) {
        case 0x0C: return Rdna2Mnemonic::GLOBAL_LOAD_DWORD;
        case 0x0D: return Rdna2Mnemonic::GLOBAL_LOAD_DWORDX2;
        case 0x0E: return Rdna2Mnemonic::GLOBAL_LOAD_DWORDX4;
        case 0x1C: return Rdna2Mnemonic::GLOBAL_STORE_DWORD;
        case 0x1D: return Rdna2Mnemonic::GLOBAL_STORE_DWORDX2;
        case 0x1E: return Rdna2Mnemonic::GLOBAL_STORE_DWORDX4;
        case 0x2C: return Rdna2Mnemonic::GLOBAL_STORE_DWORD;
        default: return Rdna2Mnemonic::Unknown;
    }
}

std::string FormatOperand(const Rdna2Operand& op) {
    std::ostringstream oss;
    if (op.neg) oss << "-";
    if (op.abs) oss << "|";
    switch (op.kind) {
        case Rdna2OperandKind::None: oss << "<none>"; break;
        case Rdna2OperandKind::Sgpr: oss << "s" << op.reg_index; break;
        case Rdna2OperandKind::Vgpr: oss << "v" << op.reg_index; break;
        case Rdna2OperandKind::VccLo: oss << "vcc_lo"; break;
        case Rdna2OperandKind::VccHi: oss << "vcc_hi"; break;
        case Rdna2OperandKind::ExecLo: oss << "exec_lo"; break;
        case Rdna2OperandKind::ExecHi: oss << "exec_hi"; break;
        case Rdna2OperandKind::M0: oss << "m0"; break;
        case Rdna2OperandKind::Scc: oss << "scc"; break;
        case Rdna2OperandKind::Null: oss << "null"; break;
        case Rdna2OperandKind::InlineInt: oss << op.imm_i32; break;
        case Rdna2OperandKind::InlineFloat: oss << op.imm_f32; break;
        case Rdna2OperandKind::Literal32: {
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "0x%08x", op.literal_u32);
            oss << buf;
            break;
        }
    }
    if (op.abs) oss << "|";
    return oss.str();
}

std::string FormatExpTarget(uint8_t tgt) {
    if (tgt <= 7) return "mrt" + std::to_string(tgt);
    if (tgt == 8) return "mrtz";
    if (tgt == 9) return "null";
    if (tgt >= 12 && tgt <= 15) return "pos" + std::to_string(tgt - 12);
    if (tgt >= 32) return "param" + std::to_string(tgt - 32);
    return "tgt" + std::to_string(tgt);
}

bool IsBranchMnemonic(Rdna2Mnemonic m) {
    switch (m) {
        case Rdna2Mnemonic::S_BRANCH:
        case Rdna2Mnemonic::S_CBRANCH_SCC0:
        case Rdna2Mnemonic::S_CBRANCH_SCC1:
        case Rdna2Mnemonic::S_CBRANCH_VCCZ:
        case Rdna2Mnemonic::S_CBRANCH_VCCNZ:
        case Rdna2Mnemonic::S_CBRANCH_EXECZ:
        case Rdna2Mnemonic::S_CBRANCH_EXECNZ:
            return true;
        default:
            return false;
    }
}

BranchCondition MapBranchCond(Rdna2Mnemonic m) {
    switch (m) {
        case Rdna2Mnemonic::S_BRANCH: return BranchCondition::Unconditional;
        case Rdna2Mnemonic::S_CBRANCH_SCC0: return BranchCondition::Scc0;
        case Rdna2Mnemonic::S_CBRANCH_SCC1: return BranchCondition::Scc1;
        case Rdna2Mnemonic::S_CBRANCH_VCCZ: return BranchCondition::VccZ;
        case Rdna2Mnemonic::S_CBRANCH_VCCNZ: return BranchCondition::VccNz;
        case Rdna2Mnemonic::S_CBRANCH_EXECZ: return BranchCondition::ExecZ;
        case Rdna2Mnemonic::S_CBRANCH_EXECNZ: return BranchCondition::ExecNz;
        default: return BranchCondition::None;
    }
}

uint32_t OperandReadU32MaxReg(const Rdna2Operand& op, bool is_sgpr) {
    if (is_sgpr && op.kind == Rdna2OperandKind::Sgpr) return op.reg_index;
    if (!is_sgpr && op.kind == Rdna2OperandKind::Vgpr) return op.reg_index;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// XS2 Container Parser & Builder
// ---------------------------------------------------------------------------
Status ParseXs2Container(const std::vector<uint8_t>& bytes, Xs2Module& out_module) {
    out_module = Xs2Module{};
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "XS2\0", 4) != 0) {
        return Status::Error(StatusCode::UnsupportedFormat, "Not an XS2 shader container");
    }
    if (bytes.size() < kXs2HeaderSize) {
        return Status::Error(StatusCode::ShaderError, "XS2 header truncated (< 32 bytes)");
    }

    Xs2Header& h = out_module.header;
    h.version = ReadU16(bytes.data() + 4);
    h.stage = static_cast<Xs2Stage>(ReadU16(bytes.data() + 6));
    const uint8_t raw_wave = bytes[8];
    h.wave_mode = (raw_wave == 64) ? Xs2WaveMode::Wave64 : Xs2WaveMode::Wave32;
    h.sgpr_count = bytes[9];
    h.vgpr_count = ReadU16(bytes.data() + 10);
    h.num_threads_x = std::max<uint16_t>(1, ReadU16(bytes.data() + 12));
    h.num_threads_y = std::max<uint16_t>(1, ReadU16(bytes.data() + 14));
    h.num_threads_z = std::max<uint16_t>(1, ReadU16(bytes.data() + 16));
    h.binding_count = ReadU16(bytes.data() + 18);
    h.code_offset = ReadU32(bytes.data() + 20);
    h.code_size = ReadU32(bytes.data() + 24);
    h.binding_offset = ReadU32(bytes.data() + 28);

    if (h.version == 0) {
        return Status::Error(StatusCode::ShaderError, "Invalid XS2 container version 0");
    }
    if (static_cast<uint16_t>(h.stage) > static_cast<uint16_t>(Xs2Stage::Amplification)) {
        return Status::Error(StatusCode::ShaderError, "Unknown XS2 shader stage");
    }

    const size_t bind_end = static_cast<size_t>(h.binding_offset) +
                            static_cast<size_t>(h.binding_count) * kXs2BindingSize;
    if (h.binding_count > 0 && (h.binding_offset < kXs2HeaderSize || bind_end > bytes.size())) {
        return Status::Error(StatusCode::ShaderError, "XS2 resource binding table out of range");
    }

    out_module.bindings.reserve(h.binding_count);
    for (uint16_t i = 0; i < h.binding_count; ++i) {
        const uint8_t* p = bytes.data() + h.binding_offset + static_cast<size_t>(i) * kXs2BindingSize;
        Xs2ResourceBinding b{};
        b.kind = static_cast<Xs2BindingKind>(p[0]);
        b.shader_register = p[1];
        b.register_space = p[2];
        b.sgpr_base = p[3];
        b.sgpr_count = p[4];
        b.format = p[5];
        b.reserved = ReadU16(p + 6);
        b.byte_stride = ReadU32(p + 8);
        b.element_count = ReadU32(p + 12);
        out_module.bindings.push_back(b);
    }

    const size_t code_end = static_cast<size_t>(h.code_offset) + static_cast<size_t>(h.code_size);
    if (h.code_size == 0 || (h.code_size % 4u) != 0 ||
        h.code_offset < kXs2HeaderSize || code_end > bytes.size()) {
        return Status::Error(StatusCode::ShaderError, "XS2 code section missing or misaligned");
    }

    out_module.raw_code_bytes.assign(bytes.begin() + h.code_offset, bytes.begin() + code_end);
    const size_t word_count = h.code_size / 4u;
    out_module.code_words.resize(word_count);
    for (size_t i = 0; i < word_count; ++i) {
        out_module.code_words[i] = ReadU32(out_module.raw_code_bytes.data() + i * 4u);
    }
    return Status::Ok();
}

std::vector<uint8_t> BuildXs2Container(
    const Xs2Header& header,
    const std::vector<Xs2ResourceBinding>& bindings,
    const std::vector<uint32_t>& code_words) {
    const uint32_t binding_off = static_cast<uint32_t>(kXs2HeaderSize);
    const uint32_t code_off = binding_off + static_cast<uint32_t>(bindings.size() * kXs2BindingSize);
    const uint32_t code_sz = static_cast<uint32_t>(code_words.size() * 4u);

    std::vector<uint8_t> out;
    out.reserve(code_off + code_sz);
    out.insert(out.end(), {'X', 'S', '2', '\0'});
    WriteU16(out, header.version == 0 ? 1 : header.version);
    WriteU16(out, static_cast<uint16_t>(header.stage));
    out.push_back(static_cast<uint8_t>(header.wave_mode));
    out.push_back(header.sgpr_count);
    WriteU16(out, header.vgpr_count);
    WriteU16(out, header.num_threads_x == 0 ? 1 : header.num_threads_x);
    WriteU16(out, header.num_threads_y == 0 ? 1 : header.num_threads_y);
    WriteU16(out, header.num_threads_z == 0 ? 1 : header.num_threads_z);
    WriteU16(out, static_cast<uint16_t>(bindings.size()));
    WriteU32(out, code_off);
    WriteU32(out, code_sz);
    WriteU32(out, binding_off);

    for (const auto& b : bindings) {
        out.push_back(static_cast<uint8_t>(b.kind));
        out.push_back(b.shader_register);
        out.push_back(b.register_space);
        out.push_back(b.sgpr_base);
        out.push_back(b.sgpr_count);
        out.push_back(b.format);
        WriteU16(out, b.reserved);
        WriteU32(out, b.byte_stride);
        WriteU32(out, b.element_count);
    }

    for (uint32_t w : code_words) {
        WriteU32(out, w);
    }
    return out;
}

// ---------------------------------------------------------------------------
// AMD RDNA2 (GFX10.3) ISA Decoder & Disassembler
// ---------------------------------------------------------------------------
const char* MnemonicName(Rdna2Mnemonic m) noexcept {
    switch (m) {
        case Rdna2Mnemonic::S_ADD_U32: return "s_add_u32";
        case Rdna2Mnemonic::S_SUB_U32: return "s_sub_u32";
        case Rdna2Mnemonic::S_ADD_I32: return "s_add_i32";
        case Rdna2Mnemonic::S_SUB_I32: return "s_sub_i32";
        case Rdna2Mnemonic::S_MIN_I32: return "s_min_i32";
        case Rdna2Mnemonic::S_MIN_U32: return "s_min_u32";
        case Rdna2Mnemonic::S_MAX_I32: return "s_max_i32";
        case Rdna2Mnemonic::S_MAX_U32: return "s_max_u32";
        case Rdna2Mnemonic::S_AND_B32: return "s_and_b32";
        case Rdna2Mnemonic::S_OR_B32: return "s_or_b32";
        case Rdna2Mnemonic::S_XOR_B32: return "s_xor_b32";
        case Rdna2Mnemonic::S_LSHL_B32: return "s_lshl_b32";
        case Rdna2Mnemonic::S_LSHR_B32: return "s_lshr_b32";
        case Rdna2Mnemonic::S_ASHR_I32: return "s_ashr_i32";
        case Rdna2Mnemonic::S_MUL_I32: return "s_mul_i32";
        case Rdna2Mnemonic::S_MOV_B32: return "s_mov_b32";
        case Rdna2Mnemonic::S_MOV_B64: return "s_mov_b64";
        case Rdna2Mnemonic::S_NOT_B32: return "s_not_b32";
        case Rdna2Mnemonic::S_WQM_B32: return "s_wqm_b32";
        case Rdna2Mnemonic::S_GETPC_B64: return "s_getpc_b64";
        case Rdna2Mnemonic::S_MOVK_I32: return "s_movk_i32";
        case Rdna2Mnemonic::S_CMOVK_I32: return "s_cmovk_i32";
        case Rdna2Mnemonic::S_CMPK_EQ_I32: return "s_cmpk_eq_i32";
        case Rdna2Mnemonic::S_CMPK_LG_I32: return "s_cmpk_lg_i32";
        case Rdna2Mnemonic::S_CMPK_GT_I32: return "s_cmpk_gt_i32";
        case Rdna2Mnemonic::S_CMPK_GE_I32: return "s_cmpk_ge_i32";
        case Rdna2Mnemonic::S_CMPK_LT_I32: return "s_cmpk_lt_i32";
        case Rdna2Mnemonic::S_CMPK_LE_I32: return "s_cmpk_le_i32";
        case Rdna2Mnemonic::S_ADDK_I32: return "s_addk_i32";
        case Rdna2Mnemonic::S_MULK_I32: return "s_mulk_i32";
        case Rdna2Mnemonic::S_CMP_EQ_I32: return "s_cmp_eq_i32";
        case Rdna2Mnemonic::S_CMP_LG_I32: return "s_cmp_lg_i32";
        case Rdna2Mnemonic::S_CMP_GT_I32: return "s_cmp_gt_i32";
        case Rdna2Mnemonic::S_CMP_GE_I32: return "s_cmp_ge_i32";
        case Rdna2Mnemonic::S_CMP_LT_I32: return "s_cmp_lt_i32";
        case Rdna2Mnemonic::S_CMP_LE_I32: return "s_cmp_le_i32";
        case Rdna2Mnemonic::S_CMP_EQ_U32: return "s_cmp_eq_u32";
        case Rdna2Mnemonic::S_CMP_LG_U32: return "s_cmp_lg_u32";
        case Rdna2Mnemonic::S_CMP_GT_U32: return "s_cmp_gt_u32";
        case Rdna2Mnemonic::S_CMP_GE_U32: return "s_cmp_ge_u32";
        case Rdna2Mnemonic::S_CMP_LT_U32: return "s_cmp_lt_u32";
        case Rdna2Mnemonic::S_CMP_LE_U32: return "s_cmp_le_u32";
        case Rdna2Mnemonic::S_NOP: return "s_nop";
        case Rdna2Mnemonic::S_ENDPGM: return "s_endpgm";
        case Rdna2Mnemonic::S_BRANCH: return "s_branch";
        case Rdna2Mnemonic::S_CBRANCH_SCC0: return "s_cbranch_scc0";
        case Rdna2Mnemonic::S_CBRANCH_SCC1: return "s_cbranch_scc1";
        case Rdna2Mnemonic::S_CBRANCH_VCCZ: return "s_cbranch_vccz";
        case Rdna2Mnemonic::S_CBRANCH_VCCNZ: return "s_cbranch_vccnz";
        case Rdna2Mnemonic::S_CBRANCH_EXECZ: return "s_cbranch_execz";
        case Rdna2Mnemonic::S_CBRANCH_EXECNZ: return "s_cbranch_execnz";
        case Rdna2Mnemonic::S_BARRIER: return "s_barrier";
        case Rdna2Mnemonic::S_WAITCNT: return "s_waitcnt";
        case Rdna2Mnemonic::V_CNDMASK_B32: return "v_cndmask_b32";
        case Rdna2Mnemonic::V_ADD_F32: return "v_add_f32";
        case Rdna2Mnemonic::V_SUB_F32: return "v_sub_f32";
        case Rdna2Mnemonic::V_MUL_F32: return "v_mul_f32";
        case Rdna2Mnemonic::V_FMAC_F32: return "v_fmac_f32";
        case Rdna2Mnemonic::V_ADD_NC_U32: return "v_add_nc_u32";
        case Rdna2Mnemonic::V_SUB_NC_U32: return "v_sub_nc_u32";
        case Rdna2Mnemonic::V_AND_B32: return "v_and_b32";
        case Rdna2Mnemonic::V_OR_B32: return "v_or_b32";
        case Rdna2Mnemonic::V_XOR_B32: return "v_xor_b32";
        case Rdna2Mnemonic::V_LSHLREV_B32: return "v_lshlrev_b32";
        case Rdna2Mnemonic::V_LSHRREV_B32: return "v_lshrrev_b32";
        case Rdna2Mnemonic::V_MIN_F32: return "v_min_f32";
        case Rdna2Mnemonic::V_MAX_F32: return "v_max_f32";
        case Rdna2Mnemonic::V_NOP: return "v_nop";
        case Rdna2Mnemonic::V_MOV_B32: return "v_mov_b32";
        case Rdna2Mnemonic::V_CVT_F32_I32: return "v_cvt_f32_i32";
        case Rdna2Mnemonic::V_CVT_F32_U32: return "v_cvt_f32_u32";
        case Rdna2Mnemonic::V_CVT_I32_F32: return "v_cvt_i32_f32";
        case Rdna2Mnemonic::V_RCP_F32: return "v_rcp_f32";
        case Rdna2Mnemonic::V_RSQ_F32: return "v_rsq_f32";
        case Rdna2Mnemonic::V_SQRT_F32: return "v_sqrt_f32";
        case Rdna2Mnemonic::V_SIN_F32: return "v_sin_f32";
        case Rdna2Mnemonic::V_COS_F32: return "v_cos_f32";
        case Rdna2Mnemonic::V_FLOOR_F32: return "v_floor_f32";
        case Rdna2Mnemonic::V_FRACT_F32: return "v_fract_f32";
        case Rdna2Mnemonic::V_CMP_LT_F32: return "v_cmp_lt_f32";
        case Rdna2Mnemonic::V_CMP_EQ_F32: return "v_cmp_eq_f32";
        case Rdna2Mnemonic::V_CMP_LE_F32: return "v_cmp_le_f32";
        case Rdna2Mnemonic::V_CMP_GT_F32: return "v_cmp_gt_f32";
        case Rdna2Mnemonic::V_CMP_NEQ_F32: return "v_cmp_neq_f32";
        case Rdna2Mnemonic::V_CMP_GE_F32: return "v_cmp_ge_f32";
        case Rdna2Mnemonic::V_CMP_LT_U32: return "v_cmp_lt_u32";
        case Rdna2Mnemonic::V_CMP_EQ_U32: return "v_cmp_eq_u32";
        case Rdna2Mnemonic::V_CMP_GT_U32: return "v_cmp_gt_u32";
        case Rdna2Mnemonic::V_MAD_F32: return "v_mad_f32";
        case Rdna2Mnemonic::V_MAD_U32_U24: return "v_mad_u32_u24";
        case Rdna2Mnemonic::V_BFE_U32: return "v_bfe_u32";
        case Rdna2Mnemonic::V_BFI_B32: return "v_bfi_b32";
        case Rdna2Mnemonic::V_FMA_F32: return "v_fma_f32";
        case Rdna2Mnemonic::V_PERM_B32: return "v_perm_b32";
        case Rdna2Mnemonic::S_LOAD_DWORD: return "s_load_dword";
        case Rdna2Mnemonic::S_LOAD_DWORDX2: return "s_load_dwordx2";
        case Rdna2Mnemonic::S_LOAD_DWORDX4: return "s_load_dwordx4";
        case Rdna2Mnemonic::S_BUFFER_LOAD_DWORD: return "s_buffer_load_dword";
        case Rdna2Mnemonic::S_BUFFER_LOAD_DWORDX2: return "s_buffer_load_dwordx2";
        case Rdna2Mnemonic::S_BUFFER_LOAD_DWORDX4: return "s_buffer_load_dwordx4";
        case Rdna2Mnemonic::BUFFER_LOAD_DWORD: return "buffer_load_dword";
        case Rdna2Mnemonic::BUFFER_LOAD_DWORDX2: return "buffer_load_dwordx2";
        case Rdna2Mnemonic::BUFFER_LOAD_DWORDX4: return "buffer_load_dwordx4";
        case Rdna2Mnemonic::BUFFER_STORE_DWORD: return "buffer_store_dword";
        case Rdna2Mnemonic::BUFFER_STORE_DWORDX2: return "buffer_store_dwordx2";
        case Rdna2Mnemonic::BUFFER_STORE_DWORDX4: return "buffer_store_dwordx4";
        case Rdna2Mnemonic::GLOBAL_LOAD_DWORD: return "global_load_dword";
        case Rdna2Mnemonic::GLOBAL_LOAD_DWORDX2: return "global_load_dwordx2";
        case Rdna2Mnemonic::GLOBAL_LOAD_DWORDX4: return "global_load_dwordx4";
        case Rdna2Mnemonic::GLOBAL_STORE_DWORD: return "global_store_dword";
        case Rdna2Mnemonic::GLOBAL_STORE_DWORDX2: return "global_store_dwordx2";
        case Rdna2Mnemonic::GLOBAL_STORE_DWORDX4: return "global_store_dwordx4";
        case Rdna2Mnemonic::EXP: return "exp";
        default: return "unknown";
    }
}

Status DecodeRdna2Instruction(
    const uint32_t* words,
    size_t word_count,
    size_t word_index,
    Rdna2Instruction& out_insn) {
    out_insn = Rdna2Instruction{};
    if (!words || word_index >= word_count) {
        return Status::Error(StatusCode::InvalidArgument, "Instruction word index out of range");
    }

    const uint32_t w0 = words[word_index];
    out_insn.byte_offset = static_cast<uint32_t>(word_index * 4u);
    out_insn.word0 = w0;

    auto read_literal = [&](size_t lit_word_idx, uint32_t& out_lit) -> Status {
        if (lit_word_idx >= word_count) {
            return Status::Error(StatusCode::ShaderError, "Truncated 32-bit literal operand");
        }
        out_lit = words[lit_word_idx];
        out_insn.has_literal = true;
        out_insn.literal = out_lit;
        out_insn.size_bytes += 4u;
        return Status::Ok();
    };

    // 1. Bit [31] == 0: VOP2 / VOP1 / VOPC (32-bit + optional literal)
    if ((w0 & 0x80000000u) == 0u) {
        const uint32_t enc7 = (w0 >> 25) & 0x7Fu;
        const uint16_t src0_raw = static_cast<uint16_t>(w0 & 0x1FFu);
        uint32_t lit = 0;
        if (src0_raw == 255u) {
            Status st = read_literal(word_index + 1, lit);
            if (!st.ok()) return st;
        }
        if (enc7 == 0x3Fu) {
            out_insn.encoding = Rdna2Encoding::Vop1;
            out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 9) & 0xFFu);
            out_insn.mnemonic = MapVop1Opcode(out_insn.raw_opcode);
            out_insn.dst = Rdna2Operand::Vgpr(static_cast<uint16_t>((w0 >> 17) & 0xFFu));
            out_insn.src0 = DecodeSrcOperand(src0_raw, true, lit);
            return Status::Ok();
        }
        if (enc7 == 0x3Eu) {
            out_insn.encoding = Rdna2Encoding::Vopc;
            out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 17) & 0xFFu);
            out_insn.mnemonic = MapVopcOpcode(out_insn.raw_opcode);
            out_insn.dst.kind = Rdna2OperandKind::VccLo;
            out_insn.src0 = DecodeSrcOperand(src0_raw, true, lit);
            out_insn.src1 = Rdna2Operand::Vgpr(static_cast<uint16_t>((w0 >> 9) & 0xFFu));
            return Status::Ok();
        }
        out_insn.encoding = Rdna2Encoding::Vop2;
        out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 25) & 0x3Fu);
        out_insn.mnemonic = MapVop2Opcode(out_insn.raw_opcode);
        out_insn.dst = Rdna2Operand::Vgpr(static_cast<uint16_t>((w0 >> 17) & 0xFFu));
        out_insn.src0 = DecodeSrcOperand(src0_raw, true, lit);
        out_insn.src1 = Rdna2Operand::Vgpr(static_cast<uint16_t>((w0 >> 9) & 0xFFu));
        return Status::Ok();
    }

    // 2. Bits [31:30] == 0b10: SOPP / SOPC / SOP1 / SOPK / SOP2
    if ((w0 >> 30) == 0x2u) {
        const uint32_t enc9 = (w0 >> 23) & 0x1FFu;
        if (enc9 == 0x17Fu) {
            out_insn.encoding = Rdna2Encoding::Sopp;
            out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            out_insn.mnemonic = MapSoppOpcode(out_insn.raw_opcode);
            out_insn.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
            return Status::Ok();
        }
        if (enc9 == 0x17Eu) {
            out_insn.encoding = Rdna2Encoding::Sopc;
            out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 16) & 0x7Fu);
            out_insn.mnemonic = MapSopcOpcode(out_insn.raw_opcode);
            const uint8_t s0 = static_cast<uint8_t>(w0 & 0xFFu);
            const uint8_t s1 = static_cast<uint8_t>((w0 >> 8) & 0xFFu);
            uint32_t lit = 0;
            if (s0 == 255u || s1 == 255u) {
                Status st = read_literal(word_index + 1, lit);
                if (!st.ok()) return st;
            }
            out_insn.dst.kind = Rdna2OperandKind::Scc;
            out_insn.src0 = DecodeSrcOperand(s0, false, lit);
            out_insn.src1 = DecodeSrcOperand(s1, false, lit);
            return Status::Ok();
        }
        if (enc9 == 0x17Du) {
            out_insn.encoding = Rdna2Encoding::Sop1;
            out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 8) & 0xFFu);
            out_insn.mnemonic = MapSop1Opcode(out_insn.raw_opcode);
            const uint8_t s0 = static_cast<uint8_t>(w0 & 0xFFu);
            uint32_t lit = 0;
            if (s0 == 255u) {
                Status st = read_literal(word_index + 1, lit);
                if (!st.ok()) return st;
            }
            out_insn.dst = DecodeScalarDst(static_cast<uint8_t>((w0 >> 16) & 0x7Fu));
            out_insn.src0 = DecodeSrcOperand(s0, false, lit);
            return Status::Ok();
        }
        if ((w0 >> 28) == 0xBu) {
            out_insn.encoding = Rdna2Encoding::Sopk;
            out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 23) & 0x1Fu);
            out_insn.mnemonic = MapSopkOpcode(out_insn.raw_opcode);
            out_insn.dst = DecodeScalarDst(static_cast<uint8_t>((w0 >> 16) & 0x7Fu));
            out_insn.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
            return Status::Ok();
        }
        out_insn.encoding = Rdna2Encoding::Sop2;
        out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 23) & 0x7Fu);
        out_insn.mnemonic = MapSop2Opcode(out_insn.raw_opcode);
        const uint8_t s0 = static_cast<uint8_t>(w0 & 0xFFu);
        const uint8_t s1 = static_cast<uint8_t>((w0 >> 8) & 0xFFu);
        uint32_t lit = 0;
        if (s0 == 255u || s1 == 255u) {
            Status st = read_literal(word_index + 1, lit);
            if (!st.ok()) return st;
        }
        out_insn.dst = DecodeScalarDst(static_cast<uint8_t>((w0 >> 16) & 0x7Fu));
        out_insn.src0 = DecodeSrcOperand(s0, false, lit);
        out_insn.src1 = DecodeSrcOperand(s1, false, lit);
        return Status::Ok();
    }

    // 3. Bits [31:30] == 0b11: 64-bit instructions (Word 0 + Word 1)
    if (word_index + 1 >= word_count) {
        return Status::Error(StatusCode::ShaderError, "Truncated 64-bit RDNA2 instruction");
    }
    const uint32_t w1 = words[word_index + 1];
    out_insn.word1 = w1;
    out_insn.size_bytes = 8u;

    const uint32_t enc6 = (w0 >> 26) & 0x3Fu;
    if (enc6 == 0x35u) {
        out_insn.encoding = Rdna2Encoding::Vop3;
        out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 16) & 0x3FFu);
        out_insn.mnemonic = MapVop3Opcode(out_insn.raw_opcode);
        out_insn.clamp = ((w0 >> 15) & 1u) != 0;
        const uint8_t abs_bits = static_cast<uint8_t>((w0 >> 8) & 0x7u);
        const uint8_t neg_bits = static_cast<uint8_t>((w1 >> 29) & 0x7u);
        out_insn.omod = static_cast<uint8_t>((w1 >> 27) & 0x3u);

        const uint16_t s0 = static_cast<uint16_t>(w1 & 0x1FFu);
        const uint16_t s1 = static_cast<uint16_t>((w1 >> 9) & 0x1FFu);
        const uint16_t s2 = static_cast<uint16_t>((w1 >> 18) & 0x1FFu);
        uint32_t lit = 0;
        if (s0 == 255u || s1 == 255u || s2 == 255u) {
            Status st = read_literal(word_index + 2, lit);
            if (!st.ok()) return st;
        }
        out_insn.dst = (out_insn.raw_opcode < 0x100u)
                           ? DecodeScalarDst(static_cast<uint8_t>(w0 & 0x7Fu))
                           : Rdna2Operand::Vgpr(static_cast<uint16_t>(w0 & 0xFFu));
        out_insn.src0 = DecodeSrcOperand(s0, true, lit);
        out_insn.src1 = DecodeSrcOperand(s1, true, lit);
        out_insn.src2 = DecodeSrcOperand(s2, true, lit);
        out_insn.src0.abs = (abs_bits & 1u) != 0;
        out_insn.src1.abs = (abs_bits & 2u) != 0;
        out_insn.src2.abs = (abs_bits & 4u) != 0;
        out_insn.src0.neg = (neg_bits & 1u) != 0;
        out_insn.src1.neg = (neg_bits & 2u) != 0;
        out_insn.src2.neg = (neg_bits & 4u) != 0;
        return Status::Ok();
    }

    if (enc6 == 0x3Du) {
        out_insn.encoding = Rdna2Encoding::Smem;
        out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 18) & 0xFFu);
        out_insn.mnemonic = MapSmemOpcode(out_insn.raw_opcode);
        out_insn.glc = ((w0 >> 16) & 1u) != 0;
        out_insn.dlc = ((w0 >> 14) & 1u) != 0;
        out_insn.dst = DecodeScalarDst(static_cast<uint8_t>((w0 >> 6) & 0x7Fu));
        out_insn.src0 = Rdna2Operand::Sgpr(static_cast<uint16_t>((w0 & 0x3Fu) * 2u));
        out_insn.src1 = DecodeSrcOperand(static_cast<uint16_t>((w1 >> 25) & 0x7Fu), false, 0);
        out_insn.offset = w1 & 0x1FFFFFu;
        return Status::Ok();
    }

    if (enc6 == 0x38u) {
        out_insn.encoding = Rdna2Encoding::Mubuf;
        out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
        out_insn.mnemonic = MapMubufOpcode(out_insn.raw_opcode);
        out_insn.slc = ((w0 >> 17) & 1u) != 0;
        out_insn.dlc = ((w0 >> 15) & 1u) != 0;
        out_insn.glc = ((w0 >> 14) & 1u) != 0;
        out_insn.idxen = ((w0 >> 13) & 1u) != 0;
        out_insn.offen = ((w0 >> 12) & 1u) != 0;
        out_insn.offset = w0 & 0xFFFu;
        out_insn.dst = Rdna2Operand::Vgpr(static_cast<uint16_t>((w1 >> 8) & 0xFFu));
        out_insn.src0 = Rdna2Operand::Vgpr(static_cast<uint16_t>(w1 & 0xFFu));
        out_insn.src1 = Rdna2Operand::Sgpr(static_cast<uint16_t>(((w1 >> 16) & 0x1Fu) * 4u));
        out_insn.src2 = DecodeSrcOperand(static_cast<uint16_t>((w1 >> 24) & 0xFFu), false, 0);
        return Status::Ok();
    }

    if (enc6 == 0x37u) {
        out_insn.encoding = Rdna2Encoding::Flat;
        out_insn.raw_opcode = static_cast<uint16_t>((w0 >> 18) & 0x7Fu);
        out_insn.mnemonic = MapFlatOpcode(out_insn.raw_opcode);
        out_insn.slc = ((w0 >> 17) & 1u) != 0;
        out_insn.glc = ((w0 >> 16) & 1u) != 0;
        out_insn.flat_seg = static_cast<uint8_t>((w0 >> 14) & 0x3u);
        out_insn.dlc = ((w0 >> 12) & 1u) != 0;
        out_insn.offset = w0 & 0xFFFu;
        out_insn.dst = Rdna2Operand::Vgpr(static_cast<uint16_t>((w1 >> 24) & 0xFFu));
        out_insn.src0 = Rdna2Operand::Vgpr(static_cast<uint16_t>(w1 & 0xFFu));
        out_insn.src1 = Rdna2Operand::Vgpr(static_cast<uint16_t>((w1 >> 8) & 0xFFu));
        out_insn.src2 = DecodeSrcOperand(static_cast<uint16_t>((w1 >> 16) & 0x7Fu), false, 0);
        return Status::Ok();
    }

    if (enc6 == 0x3Eu) {
        out_insn.encoding = Rdna2Encoding::Exp;
        out_insn.mnemonic = Rdna2Mnemonic::EXP;
        out_insn.exp_en = static_cast<uint8_t>(w0 & 0xFu);
        out_insn.exp_target = static_cast<uint8_t>((w0 >> 4) & 0x3Fu);
        out_insn.exp_compr = ((w0 >> 10) & 1u) != 0;
        out_insn.exp_done = ((w0 >> 11) & 1u) != 0;
        out_insn.exp_vm = ((w0 >> 12) & 1u) != 0;
        out_insn.src0 = Rdna2Operand::Vgpr(static_cast<uint16_t>(w1 & 0xFFu));
        out_insn.src1 = Rdna2Operand::Vgpr(static_cast<uint16_t>((w1 >> 8) & 0xFFu));
        out_insn.src2 = Rdna2Operand::Vgpr(static_cast<uint16_t>((w1 >> 16) & 0xFFu));
        out_insn.src3 = Rdna2Operand::Vgpr(static_cast<uint16_t>((w1 >> 24) & 0xFFu));
        return Status::Ok();
    }

    return Status::Error(StatusCode::ShaderError, "Unrecognized RDNA2 instruction encoding");
}

Status DecodeRdna2Program(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_insns) {
    out_insns.clear();
    size_t idx = 0;
    while (idx < words.size()) {
        Rdna2Instruction insn{};
        Status st = DecodeRdna2Instruction(words.data(), words.size(), idx, insn);
        if (!st.ok()) {
            return st;
        }
        idx += insn.size_bytes / 4u;
        out_insns.push_back(std::move(insn));
    }
    return Status::Ok();
}

std::string FormatRdna2Instruction(const Rdna2Instruction& insn) {
    std::ostringstream oss;
    oss << MnemonicName(insn.mnemonic);
    switch (insn.encoding) {
        case Rdna2Encoding::Sop2:
        case Rdna2Encoding::Vop2:
            oss << " " << FormatOperand(insn.dst) << ", " << FormatOperand(insn.src0)
                << ", " << FormatOperand(insn.src1);
            break;
        case Rdna2Encoding::Sop1:
        case Rdna2Encoding::Vop1:
            oss << " " << FormatOperand(insn.dst) << ", " << FormatOperand(insn.src0);
            break;
        case Rdna2Encoding::Sopk:
            oss << " " << FormatOperand(insn.dst) << ", " << insn.simm16;
            break;
        case Rdna2Encoding::Sopc:
            oss << " " << FormatOperand(insn.src0) << ", " << FormatOperand(insn.src1);
            break;
        case Rdna2Encoding::Sopp:
            if (insn.mnemonic != Rdna2Mnemonic::S_ENDPGM &&
                insn.mnemonic != Rdna2Mnemonic::S_BARRIER) {
                oss << " " << insn.simm16;
            }
            break;
        case Rdna2Encoding::Vopc:
            oss << " " << FormatOperand(insn.dst) << ", " << FormatOperand(insn.src0)
                << ", " << FormatOperand(insn.src1);
            break;
        case Rdna2Encoding::Vop3:
            oss << " " << FormatOperand(insn.dst) << ", " << FormatOperand(insn.src0)
                << ", " << FormatOperand(insn.src1);
            if (insn.raw_opcode >= 0x1C0u) {
                oss << ", " << FormatOperand(insn.src2);
            }
            break;
        case Rdna2Encoding::Smem:
            oss << " " << FormatOperand(insn.dst) << ", s[" << insn.src0.reg_index << ":"
                << (insn.src0.reg_index + 3) << "], " << FormatOperand(insn.src1);
            if (insn.offset != 0) oss << " offset:" << insn.offset;
            break;
        case Rdna2Encoding::Mubuf:
            oss << " " << FormatOperand(insn.dst) << ", " << FormatOperand(insn.src0)
                << ", s[" << insn.src1.reg_index << ":" << (insn.src1.reg_index + 3) << "], "
                << FormatOperand(insn.src2);
            if (insn.offen) oss << " offen";
            if (insn.idxen) oss << " idxen";
            if (insn.offset != 0) oss << " offset:" << insn.offset;
            break;
        case Rdna2Encoding::Flat:
            oss << " " << FormatOperand(insn.dst) << ", " << FormatOperand(insn.src0)
                << ", " << FormatOperand(insn.src2);
            if (insn.offset != 0) oss << " offset:" << insn.offset;
            break;
        case Rdna2Encoding::Exp:
            oss << " " << FormatExpTarget(insn.exp_target) << ", "
                << ((insn.exp_en & 1u) ? FormatOperand(insn.src0) : "off") << ", "
                << ((insn.exp_en & 2u) ? FormatOperand(insn.src1) : "off") << ", "
                << ((insn.exp_en & 4u) ? FormatOperand(insn.src2) : "off") << ", "
                << ((insn.exp_en & 8u) ? FormatOperand(insn.src3) : "off");
            if (insn.exp_done) oss << " done";
            if (insn.exp_vm) oss << " vm";
            break;
        default:
            break;
    }
    return oss.str();
}

std::string DisassembleRdna2(const std::vector<Rdna2Instruction>& insns) {
    std::ostringstream oss;
    for (const auto& insn : insns) {
        char addr[16]{};
        std::snprintf(addr, sizeof(addr), "%04X: ", insn.byte_offset);
        oss << addr << FormatRdna2Instruction(insn) << "\n";
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// Control-Flow Graph Reconstruction
// ---------------------------------------------------------------------------
Status BuildRdna2Cfg(const std::vector<Rdna2Instruction>& insns, Rdna2Cfg& out_cfg) {
    out_cfg.blocks.clear();
    if (insns.empty()) {
        return Status::Error(StatusCode::ShaderError, "Cannot build CFG for empty RDNA2 program");
    }

    std::map<uint32_t, size_t> byte_to_insn;
    for (size_t i = 0; i < insns.size(); ++i) {
        byte_to_insn[insns[i].byte_offset] = i;
    }

    std::set<size_t> leaders;
    leaders.insert(0);

    for (size_t i = 0; i < insns.size(); ++i) {
        const auto& insn = insns[i];
        if (IsBranchMnemonic(insn.mnemonic)) {
            const int64_t target = static_cast<int64_t>(insn.byte_offset) + 4 +
                                   static_cast<int64_t>(insn.simm16) * 4;
            if (target >= 0) {
                auto it = byte_to_insn.find(static_cast<uint32_t>(target));
                if (it != byte_to_insn.end()) {
                    leaders.insert(it->second);
                }
            }
            if (i + 1 < insns.size()) {
                leaders.insert(i + 1);
            }
        } else if (insn.mnemonic == Rdna2Mnemonic::S_ENDPGM && i + 1 < insns.size()) {
            leaders.insert(i + 1);
        }
    }

    std::vector<size_t> sorted_leaders(leaders.begin(), leaders.end());
    std::map<uint32_t, uint32_t> byte_to_block;
    out_cfg.blocks.resize(sorted_leaders.size());

    for (size_t b = 0; b < sorted_leaders.size(); ++b) {
        const size_t start_idx = sorted_leaders[b];
        const size_t end_idx = (b + 1 < sorted_leaders.size()) ? sorted_leaders[b + 1] : insns.size();
        Rdna2BasicBlock& bb = out_cfg.blocks[b];
        bb.id = static_cast<uint32_t>(b);
        bb.start_byte = insns[start_idx].byte_offset;
        bb.end_byte = insns[end_idx - 1].byte_offset + insns[end_idx - 1].size_bytes;
        for (size_t k = start_idx; k < end_idx; ++k) {
            bb.instruction_indices.push_back(k);
        }
        byte_to_block[bb.start_byte] = bb.id;
    }

    for (size_t b = 0; b < out_cfg.blocks.size(); ++b) {
        Rdna2BasicBlock& bb = out_cfg.blocks[b];
        const auto& last = insns[bb.instruction_indices.back()];
        if (last.mnemonic == Rdna2Mnemonic::S_ENDPGM) {
            bb.terminates = true;
            continue;
        }
        if (IsBranchMnemonic(last.mnemonic)) {
            bb.branch_cond = MapBranchCond(last.mnemonic);
            const int64_t target = static_cast<int64_t>(last.byte_offset) + 4 +
                                   static_cast<int64_t>(last.simm16) * 4;
            if (target >= 0) {
                auto it = byte_to_block.find(static_cast<uint32_t>(target));
                if (it != byte_to_block.end()) {
                    bb.branch_target_block = static_cast<int32_t>(it->second);
                }
            }
            if (bb.branch_cond != BranchCondition::Unconditional && b + 1 < out_cfg.blocks.size()) {
                bb.fallthrough_block = static_cast<int32_t>(b + 1);
            }
        } else if (b + 1 < out_cfg.blocks.size()) {
            bb.fallthrough_block = static_cast<int32_t>(b + 1);
        } else {
            bb.terminates = true;
        }
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// HLSL Lifter & DXBC Compiler
// ---------------------------------------------------------------------------
namespace {

const Xs2ResourceBinding* FindBindingBySgpr(const Xs2Module& mod, uint32_t sgpr_idx) {
    for (const auto& b : mod.bindings) {
        if (b.kind == Xs2BindingKind::Cbv || b.kind == Xs2BindingKind::Srv ||
            b.kind == Xs2BindingKind::Uav) {
            if (sgpr_idx >= b.sgpr_base && sgpr_idx < static_cast<uint32_t>(b.sgpr_base) + b.sgpr_count) {
                return &b;
            }
        }
    }
    return nullptr;
}

std::string HlslSrcExpr(const Rdna2Operand& op) {
    std::string base;
    switch (op.kind) {
        case Rdna2OperandKind::Sgpr:
            base = "s[" + std::to_string(op.reg_index) + "]";
            break;
        case Rdna2OperandKind::Vgpr:
            base = "v[" + std::to_string(op.reg_index) + "]";
            break;
        case Rdna2OperandKind::VccLo:
            base = "(vcc ? 1u : 0u)";
            break;
        case Rdna2OperandKind::VccHi:
            base = "0u";
            break;
        case Rdna2OperandKind::ExecLo:
            base = "(exec_active ? 1u : 0u)";
            break;
        case Rdna2OperandKind::ExecHi:
            base = "0u";
            break;
        case Rdna2OperandKind::M0:
            base = "m0_reg";
            break;
        case Rdna2OperandKind::Scc:
            base = "(scc ? 1u : 0u)";
            break;
        case Rdna2OperandKind::Null:
            base = "0u";
            break;
        case Rdna2OperandKind::InlineInt:
            base = std::to_string(static_cast<uint32_t>(op.imm_i32)) + "u";
            break;
        case Rdna2OperandKind::InlineFloat:
            base = std::to_string(FloatToBits(op.imm_f32)) + "u";
            break;
        case Rdna2OperandKind::Literal32:
            base = std::to_string(op.literal_u32) + "u";
            break;
        default:
            base = "0u";
            break;
    }
    if (op.abs) {
        base = "asuint(abs(asfloat(" + base + ")))";
    }
    if (op.neg) {
        base = "asuint(-asfloat(" + base + "))";
    }
    return base;
}

std::string HlslDstLhs(const Rdna2Operand& op) {
    if (op.kind == Rdna2OperandKind::Sgpr) return "s[" + std::to_string(op.reg_index) + "]";
    if (op.kind == Rdna2OperandKind::Vgpr) return "v[" + std::to_string(op.reg_index) + "]";
    if (op.kind == Rdna2OperandKind::M0) return "m0_reg";
    return "_discard_u32";
}

void EmitHlslInstruction(
    std::ostringstream& out,
    const Xs2Module& mod,
    const Rdna2Instruction& insn,
    const std::string& indent) {
    auto emit_float_bin = [&](const char* op_str) {
        out << indent << HlslDstLhs(insn.dst) << " = asuint(asfloat(" << HlslSrcExpr(insn.src0)
            << ") " << op_str << " asfloat(" << HlslSrcExpr(insn.src1) << "));\n";
    };
    auto emit_uint_bin = [&](const char* op_str) {
        out << indent << HlslDstLhs(insn.dst) << " = (" << HlslSrcExpr(insn.src0) << " " << op_str
            << " " << HlslSrcExpr(insn.src1) << ");\n";
    };

    switch (insn.mnemonic) {
        case Rdna2Mnemonic::S_NOP:
        case Rdna2Mnemonic::V_NOP:
        case Rdna2Mnemonic::S_WAITCNT:
            break;
        case Rdna2Mnemonic::S_BARRIER:
            if (mod.header.stage == Xs2Stage::Compute) {
                out << indent << "GroupMemoryBarrierWithGroupSync();\n";
            }
            break;
        case Rdna2Mnemonic::S_MOV_B32:
            if (insn.dst.kind != Rdna2OperandKind::Null) {
                out << indent << HlslDstLhs(insn.dst) << " = " << HlslSrcExpr(insn.src0) << ";\n";
            }
            break;
        case Rdna2Mnemonic::S_NOT_B32:
            out << indent << HlslDstLhs(insn.dst) << " = ~" << HlslSrcExpr(insn.src0)
                << "; scc = (" << HlslDstLhs(insn.dst) << " != 0u);\n";
            break;
        case Rdna2Mnemonic::S_MOVK_I32:
            out << indent << HlslDstLhs(insn.dst) << " = "
                << static_cast<uint32_t>(static_cast<int32_t>(insn.simm16)) << "u;\n";
            break;
        case Rdna2Mnemonic::S_ADDK_I32:
            out << indent << HlslDstLhs(insn.dst) << " = " << HlslDstLhs(insn.dst) << " + "
                << static_cast<uint32_t>(static_cast<int32_t>(insn.simm16)) << "u;\n";
            break;
        case Rdna2Mnemonic::S_MULK_I32:
            out << indent << HlslDstLhs(insn.dst) << " = " << HlslDstLhs(insn.dst) << " * "
                << static_cast<uint32_t>(static_cast<int32_t>(insn.simm16)) << "u;\n";
            break;
        case Rdna2Mnemonic::S_ADD_U32:
        case Rdna2Mnemonic::S_ADD_I32:
            out << indent << "{ uint _a = " << HlslSrcExpr(insn.src0) << "; uint _b = "
                << HlslSrcExpr(insn.src1) << "; uint _r = _a + _b; scc = (_r < _a); "
                << HlslDstLhs(insn.dst) << " = _r; }\n";
            break;
        case Rdna2Mnemonic::S_SUB_U32:
        case Rdna2Mnemonic::S_SUB_I32:
            out << indent << "{ uint _a = " << HlslSrcExpr(insn.src0) << "; uint _b = "
                << HlslSrcExpr(insn.src1) << "; scc = (_a < _b); "
                << HlslDstLhs(insn.dst) << " = _a - _b; }\n";
            break;
        case Rdna2Mnemonic::S_AND_B32:
            out << indent << HlslDstLhs(insn.dst) << " = (" << HlslSrcExpr(insn.src0) << " & "
                << HlslSrcExpr(insn.src1) << "); scc = (" << HlslDstLhs(insn.dst) << " != 0u);\n";
            break;
        case Rdna2Mnemonic::S_OR_B32:
            out << indent << HlslDstLhs(insn.dst) << " = (" << HlslSrcExpr(insn.src0) << " | "
                << HlslSrcExpr(insn.src1) << "); scc = (" << HlslDstLhs(insn.dst) << " != 0u);\n";
            break;
        case Rdna2Mnemonic::S_XOR_B32:
            out << indent << HlslDstLhs(insn.dst) << " = (" << HlslSrcExpr(insn.src0) << " ^ "
                << HlslSrcExpr(insn.src1) << "); scc = (" << HlslDstLhs(insn.dst) << " != 0u);\n";
            break;
        case Rdna2Mnemonic::S_LSHL_B32:
            out << indent << HlslDstLhs(insn.dst) << " = (" << HlslSrcExpr(insn.src0) << " << ("
                << HlslSrcExpr(insn.src1) << " & 31u)); scc = (" << HlslDstLhs(insn.dst) << " != 0u);\n";
            break;
        case Rdna2Mnemonic::S_LSHR_B32:
            out << indent << HlslDstLhs(insn.dst) << " = (" << HlslSrcExpr(insn.src0) << " >> ("
                << HlslSrcExpr(insn.src1) << " & 31u)); scc = (" << HlslDstLhs(insn.dst) << " != 0u);\n";
            break;
        case Rdna2Mnemonic::S_ASHR_I32:
            out << indent << HlslDstLhs(insn.dst) << " = uint(int(" << HlslSrcExpr(insn.src0)
                << ") >> int(" << HlslSrcExpr(insn.src1) << " & 31u));\n";
            break;
        case Rdna2Mnemonic::S_MUL_I32:
            emit_uint_bin("*");
            break;
        case Rdna2Mnemonic::S_MIN_I32:
            out << indent << HlslDstLhs(insn.dst) << " = uint(min(int(" << HlslSrcExpr(insn.src0)
                << "), int(" << HlslSrcExpr(insn.src1) << ")));\n";
            break;
        case Rdna2Mnemonic::S_MIN_U32:
            out << indent << HlslDstLhs(insn.dst) << " = min(" << HlslSrcExpr(insn.src0)
                << ", " << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::S_MAX_I32:
            out << indent << HlslDstLhs(insn.dst) << " = uint(max(int(" << HlslSrcExpr(insn.src0)
                << "), int(" << HlslSrcExpr(insn.src1) << ")));\n";
            break;
        case Rdna2Mnemonic::S_MAX_U32:
            out << indent << HlslDstLhs(insn.dst) << " = max(" << HlslSrcExpr(insn.src0)
                << ", " << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::S_CMP_EQ_I32:
            out << indent << "scc = (int(" << HlslSrcExpr(insn.src0) << ") == int("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::S_CMP_LG_I32:
            out << indent << "scc = (int(" << HlslSrcExpr(insn.src0) << ") != int("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::S_CMP_GT_I32:
            out << indent << "scc = (int(" << HlslSrcExpr(insn.src0) << ") > int("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::S_CMP_GE_I32:
            out << indent << "scc = (int(" << HlslSrcExpr(insn.src0) << ") >= int("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::S_CMP_LT_I32:
            out << indent << "scc = (int(" << HlslSrcExpr(insn.src0) << ") < int("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::S_CMP_LE_I32:
            out << indent << "scc = (int(" << HlslSrcExpr(insn.src0) << ") <= int("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::S_CMP_EQ_U32:
            out << indent << "scc = (" << HlslSrcExpr(insn.src0) << " == "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::S_CMP_LG_U32:
            out << indent << "scc = (" << HlslSrcExpr(insn.src0) << " != "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::S_CMP_GT_U32:
            out << indent << "scc = (" << HlslSrcExpr(insn.src0) << " > "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::S_CMP_GE_U32:
            out << indent << "scc = (" << HlslSrcExpr(insn.src0) << " >= "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::S_CMP_LT_U32:
            out << indent << "scc = (" << HlslSrcExpr(insn.src0) << " < "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::S_CMP_LE_U32:
            out << indent << "scc = (" << HlslSrcExpr(insn.src0) << " <= "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::V_MOV_B32:
            out << indent << HlslDstLhs(insn.dst) << " = " << HlslSrcExpr(insn.src0) << ";\n";
            break;
        case Rdna2Mnemonic::V_CNDMASK_B32:
            out << indent << HlslDstLhs(insn.dst) << " = (vcc ? " << HlslSrcExpr(insn.src1)
                << " : " << HlslSrcExpr(insn.src0) << ");\n";
            break;
        case Rdna2Mnemonic::V_ADD_F32: emit_float_bin("+"); break;
        case Rdna2Mnemonic::V_SUB_F32: emit_float_bin("-"); break;
        case Rdna2Mnemonic::V_MUL_F32: emit_float_bin("*"); break;
        case Rdna2Mnemonic::V_FMAC_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(asfloat(" << HlslSrcExpr(insn.src0)
                << ") * asfloat(" << HlslSrcExpr(insn.src1) << ") + asfloat("
                << HlslDstLhs(insn.dst) << "));\n";
            break;
        case Rdna2Mnemonic::V_ADD_NC_U32: emit_uint_bin("+"); break;
        case Rdna2Mnemonic::V_SUB_NC_U32: emit_uint_bin("-"); break;
        case Rdna2Mnemonic::V_AND_B32: emit_uint_bin("&"); break;
        case Rdna2Mnemonic::V_OR_B32: emit_uint_bin("|"); break;
        case Rdna2Mnemonic::V_XOR_B32: emit_uint_bin("^"); break;
        case Rdna2Mnemonic::V_LSHLREV_B32:
            out << indent << HlslDstLhs(insn.dst) << " = (" << HlslSrcExpr(insn.src1) << " << ("
                << HlslSrcExpr(insn.src0) << " & 31u));\n";
            break;
        case Rdna2Mnemonic::V_LSHRREV_B32:
            out << indent << HlslDstLhs(insn.dst) << " = (" << HlslSrcExpr(insn.src1) << " >> ("
                << HlslSrcExpr(insn.src0) << " & 31u));\n";
            break;
        case Rdna2Mnemonic::V_MIN_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(min(asfloat("
                << HlslSrcExpr(insn.src0) << "), asfloat(" << HlslSrcExpr(insn.src1) << ")));\n";
            break;
        case Rdna2Mnemonic::V_MAX_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(max(asfloat("
                << HlslSrcExpr(insn.src0) << "), asfloat(" << HlslSrcExpr(insn.src1) << ")));\n";
            break;
        case Rdna2Mnemonic::V_CVT_F32_I32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(float(int("
                << HlslSrcExpr(insn.src0) << ")));\n";
            break;
        case Rdna2Mnemonic::V_CVT_F32_U32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(float("
                << HlslSrcExpr(insn.src0) << "));\n";
            break;
        case Rdna2Mnemonic::V_CVT_I32_F32:
            out << indent << HlslDstLhs(insn.dst) << " = uint(int(asfloat("
                << HlslSrcExpr(insn.src0) << ")));\n";
            break;
        case Rdna2Mnemonic::V_RCP_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(1.0f / asfloat("
                << HlslSrcExpr(insn.src0) << "));\n";
            break;
        case Rdna2Mnemonic::V_RSQ_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(rsqrt(asfloat("
                << HlslSrcExpr(insn.src0) << ")));\n";
            break;
        case Rdna2Mnemonic::V_SQRT_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(sqrt(asfloat("
                << HlslSrcExpr(insn.src0) << ")));\n";
            break;
        case Rdna2Mnemonic::V_SIN_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(sin(asfloat("
                << HlslSrcExpr(insn.src0) << ") * 6.28318530718f));\n";
            break;
        case Rdna2Mnemonic::V_COS_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(cos(asfloat("
                << HlslSrcExpr(insn.src0) << ") * 6.28318530718f));\n";
            break;
        case Rdna2Mnemonic::V_FLOOR_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(floor(asfloat("
                << HlslSrcExpr(insn.src0) << ")));\n";
            break;
        case Rdna2Mnemonic::V_FRACT_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(frac(asfloat("
                << HlslSrcExpr(insn.src0) << ")));\n";
            break;
        case Rdna2Mnemonic::V_CMP_LT_F32:
            out << indent << "vcc = (asfloat(" << HlslSrcExpr(insn.src0) << ") < asfloat("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::V_CMP_EQ_F32:
            out << indent << "vcc = (asfloat(" << HlslSrcExpr(insn.src0) << ") == asfloat("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::V_CMP_LE_F32:
            out << indent << "vcc = (asfloat(" << HlslSrcExpr(insn.src0) << ") <= asfloat("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::V_CMP_GT_F32:
            out << indent << "vcc = (asfloat(" << HlslSrcExpr(insn.src0) << ") > asfloat("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::V_CMP_NEQ_F32:
            out << indent << "vcc = (asfloat(" << HlslSrcExpr(insn.src0) << ") != asfloat("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::V_CMP_GE_F32:
            out << indent << "vcc = (asfloat(" << HlslSrcExpr(insn.src0) << ") >= asfloat("
                << HlslSrcExpr(insn.src1) << "));\n";
            break;
        case Rdna2Mnemonic::V_CMP_LT_U32:
            out << indent << "vcc = (" << HlslSrcExpr(insn.src0) << " < "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::V_CMP_EQ_U32:
            out << indent << "vcc = (" << HlslSrcExpr(insn.src0) << " == "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::V_CMP_GT_U32:
            out << indent << "vcc = (" << HlslSrcExpr(insn.src0) << " > "
                << HlslSrcExpr(insn.src1) << ");\n";
            break;
        case Rdna2Mnemonic::V_MAD_F32:
        case Rdna2Mnemonic::V_FMA_F32:
            out << indent << HlslDstLhs(insn.dst) << " = asuint(asfloat(" << HlslSrcExpr(insn.src0)
                << ") * asfloat(" << HlslSrcExpr(insn.src1) << ") + asfloat("
                << HlslSrcExpr(insn.src2) << "));\n";
            break;
        case Rdna2Mnemonic::V_MAD_U32_U24:
            out << indent << HlslDstLhs(insn.dst) << " = ((" << HlslSrcExpr(insn.src0)
                << " & 0xFFFFFFu) * (" << HlslSrcExpr(insn.src1) << " & 0xFFFFFFu) + "
                << HlslSrcExpr(insn.src2) << ");\n";
            break;
        case Rdna2Mnemonic::V_BFE_U32:
            out << indent << "{ uint _w = (" << HlslSrcExpr(insn.src2)
                << " & 31u); uint _m = (_w == 0u) ? 0u : (0xFFFFFFFFu >> (32u - _w)); "
                << HlslDstLhs(insn.dst) << " = ((" << HlslSrcExpr(insn.src0) << " >> ("
                << HlslSrcExpr(insn.src1) << " & 31u)) & _m); }\n";
            break;
        case Rdna2Mnemonic::V_BFI_B32:
            out << indent << HlslDstLhs(insn.dst) << " = ((" << HlslSrcExpr(insn.src0) << " & "
                << HlslSrcExpr(insn.src1) << ") | (~" << HlslSrcExpr(insn.src0) << " & "
                << HlslSrcExpr(insn.src2) << "));\n";
            break;
        case Rdna2Mnemonic::V_PERM_B32:
            out << indent << HlslDstLhs(insn.dst) << " = _rdna2_perm(" << HlslSrcExpr(insn.src0)
                << ", " << HlslSrcExpr(insn.src1) << ", " << HlslSrcExpr(insn.src2) << ");\n";
            break;
        case Rdna2Mnemonic::S_LOAD_DWORD:
        case Rdna2Mnemonic::S_LOAD_DWORDX2:
        case Rdna2Mnemonic::S_LOAD_DWORDX4:
        case Rdna2Mnemonic::S_BUFFER_LOAD_DWORD:
        case Rdna2Mnemonic::S_BUFFER_LOAD_DWORDX2:
        case Rdna2Mnemonic::S_BUFFER_LOAD_DWORDX4: {
            const uint32_t lanes =
                (insn.mnemonic == Rdna2Mnemonic::S_LOAD_DWORDX4 ||
                 insn.mnemonic == Rdna2Mnemonic::S_BUFFER_LOAD_DWORDX4)
                    ? 4u
                    : ((insn.mnemonic == Rdna2Mnemonic::S_LOAD_DWORDX2 ||
                        insn.mnemonic == Rdna2Mnemonic::S_BUFFER_LOAD_DWORDX2)
                           ? 2u
                           : 1u);
            const Xs2ResourceBinding* b = FindBindingBySgpr(mod, insn.src0.reg_index);
            const std::string addr_expr =
                "(" + std::to_string(insn.offset) + "u + " + HlslSrcExpr(insn.src1) + ")";
            for (uint32_t k = 0; k < lanes; ++k) {
                const std::string dst_reg = "s[" + std::to_string(insn.dst.reg_index + k) + "]";
                if (b && b->kind == Xs2BindingKind::Cbv) {
                    out << indent << dst_reg << " = cb_b" << static_cast<uint32_t>(b->shader_register)
                        << "[((" << addr_expr << ") >> 2u) + " << k << "u];\n";
                } else if (b && b->kind == Xs2BindingKind::Srv) {
                    out << indent << dst_reg << " = srv_t" << static_cast<uint32_t>(b->shader_register)
                        << ".Load((" << addr_expr << ") + " << (k * 4u) << "u);\n";
                } else if (b && b->kind == Xs2BindingKind::Uav) {
                    out << indent << dst_reg << " = uav_u" << static_cast<uint32_t>(b->shader_register)
                        << ".Load((" << addr_expr << ") + " << (k * 4u) << "u);\n";
                } else {
                    out << indent << dst_reg << " = 0u;\n";
                }
            }
            break;
        }
        case Rdna2Mnemonic::BUFFER_LOAD_DWORD:
        case Rdna2Mnemonic::BUFFER_LOAD_DWORDX2:
        case Rdna2Mnemonic::BUFFER_LOAD_DWORDX4:
        case Rdna2Mnemonic::BUFFER_STORE_DWORD:
        case Rdna2Mnemonic::BUFFER_STORE_DWORDX2:
        case Rdna2Mnemonic::BUFFER_STORE_DWORDX4: {
            const bool is_store =
                insn.mnemonic == Rdna2Mnemonic::BUFFER_STORE_DWORD ||
                insn.mnemonic == Rdna2Mnemonic::BUFFER_STORE_DWORDX2 ||
                insn.mnemonic == Rdna2Mnemonic::BUFFER_STORE_DWORDX4;
            const uint32_t lanes =
                (insn.mnemonic == Rdna2Mnemonic::BUFFER_LOAD_DWORDX4 ||
                 insn.mnemonic == Rdna2Mnemonic::BUFFER_STORE_DWORDX4)
                    ? 4u
                    : ((insn.mnemonic == Rdna2Mnemonic::BUFFER_LOAD_DWORDX2 ||
                        insn.mnemonic == Rdna2Mnemonic::BUFFER_STORE_DWORDX2)
                           ? 2u
                           : 1u);
            const Xs2ResourceBinding* b = FindBindingBySgpr(mod, insn.src1.reg_index);
            std::string addr = std::to_string(insn.offset) + "u + " + HlslSrcExpr(insn.src2);
            if (insn.offen) {
                addr += " + v[" + std::to_string(insn.src0.reg_index) + "]";
            } else if (insn.idxen && b) {
                addr += " + v[" + std::to_string(insn.src0.reg_index) + "] * " +
                        std::to_string(b->byte_stride == 0 ? 4u : b->byte_stride) + "u";
            }
            for (uint32_t k = 0; k < lanes; ++k) {
                const std::string vreg = "v[" + std::to_string(insn.dst.reg_index + k) + "]";
                const std::string byte_off = "(" + addr + ") + " + std::to_string(k * 4u) + "u";
                if (is_store) {
                    if (b && b->kind == Xs2BindingKind::Uav) {
                        out << indent << "uav_u" << static_cast<uint32_t>(b->shader_register)
                            << ".Store(" << byte_off << ", " << vreg << ");\n";
                    }
                } else {
                    if (b && b->kind == Xs2BindingKind::Srv) {
                        out << indent << vreg << " = srv_t"
                            << static_cast<uint32_t>(b->shader_register) << ".Load(" << byte_off << ");\n";
                    } else if (b && b->kind == Xs2BindingKind::Uav) {
                        out << indent << vreg << " = uav_u"
                            << static_cast<uint32_t>(b->shader_register) << ".Load(" << byte_off << ");\n";
                    } else if (b && b->kind == Xs2BindingKind::Cbv) {
                        out << indent << vreg << " = cb_b"
                            << static_cast<uint32_t>(b->shader_register) << "[((" << byte_off << ") >> 2u)];\n";
                    } else {
                        out << indent << vreg << " = 0u;\n";
                    }
                }
            }
            break;
        }
        case Rdna2Mnemonic::GLOBAL_LOAD_DWORD:
        case Rdna2Mnemonic::GLOBAL_LOAD_DWORDX2:
        case Rdna2Mnemonic::GLOBAL_LOAD_DWORDX4:
        case Rdna2Mnemonic::GLOBAL_STORE_DWORD:
        case Rdna2Mnemonic::GLOBAL_STORE_DWORDX2:
        case Rdna2Mnemonic::GLOBAL_STORE_DWORDX4: {
            const bool is_store =
                insn.mnemonic == Rdna2Mnemonic::GLOBAL_STORE_DWORD ||
                insn.mnemonic == Rdna2Mnemonic::GLOBAL_STORE_DWORDX2 ||
                insn.mnemonic == Rdna2Mnemonic::GLOBAL_STORE_DWORDX4;
            const Xs2ResourceBinding* fallback_uav = nullptr;
            const Xs2ResourceBinding* fallback_srv = nullptr;
            for (const auto& b : mod.bindings) {
                if (b.kind == Xs2BindingKind::Uav && !fallback_uav) fallback_uav = &b;
                if (b.kind == Xs2BindingKind::Srv && !fallback_srv) fallback_srv = &b;
            }
            const std::string addr =
                "v[" + std::to_string(insn.src0.reg_index) + "] + " + std::to_string(insn.offset) + "u";
            if (is_store && fallback_uav) {
                out << indent << "uav_u" << static_cast<uint32_t>(fallback_uav->shader_register)
                    << ".Store(" << addr << ", v[" << insn.src1.reg_index << "]);\n";
            } else if (!is_store && fallback_srv) {
                out << indent << "v[" << insn.dst.reg_index << "] = srv_t"
                    << static_cast<uint32_t>(fallback_srv->shader_register) << ".Load(" << addr << ");\n";
            } else if (!is_store && fallback_uav) {
                out << indent << "v[" << insn.dst.reg_index << "] = uav_u"
                    << static_cast<uint32_t>(fallback_uav->shader_register) << ".Load(" << addr << ");\n";
            }
            break;
        }
        case Rdna2Mnemonic::EXP: {
            std::string target_expr;
            if (insn.exp_target >= 12 && insn.exp_target <= 15) {
                target_expr = (insn.exp_target == 12) ? "_out.pos"
                                                      : ("_out.pos" + std::to_string(insn.exp_target - 12));
            } else if (insn.exp_target <= 7) {
                target_expr = "_out.mrt" + std::to_string(insn.exp_target);
            } else if (insn.exp_target >= 32) {
                target_expr = "_out.param" + std::to_string(insn.exp_target - 32);
            }
            if (!target_expr.empty()) {
                out << indent << target_expr << " = float4("
                    << ((insn.exp_en & 1u) ? ("asfloat(v[" + std::to_string(insn.src0.reg_index) + "])") : "0.0f")
                    << ", "
                    << ((insn.exp_en & 2u) ? ("asfloat(v[" + std::to_string(insn.src1.reg_index) + "])") : "0.0f")
                    << ", "
                    << ((insn.exp_en & 4u) ? ("asfloat(v[" + std::to_string(insn.src2.reg_index) + "])") : "0.0f")
                    << ", "
                    << ((insn.exp_en & 8u) ? ("asfloat(v[" + std::to_string(insn.src3.reg_index) + "])") : "1.0f")
                    << ");\n";
            }
            break;
        }
        default:
            break;
    }
}

void EmitBranchFooter(std::ostringstream& out, const Rdna2BasicBlock& bb, const std::string& indent) {
    if (bb.terminates) {
        out << indent << "_pc_block = 0xFFFFFFFFu;\n";
        return;
    }
    if (bb.branch_cond == BranchCondition::None) {
        out << indent << "_pc_block = "
            << (bb.fallthrough_block >= 0 ? (std::to_string(bb.fallthrough_block) + "u") : "0xFFFFFFFFu")
            << ";\n";
        return;
    }
    if (bb.branch_cond == BranchCondition::Unconditional) {
        out << indent << "_pc_block = "
            << (bb.branch_target_block >= 0 ? (std::to_string(bb.branch_target_block) + "u") : "0xFFFFFFFFu")
            << ";\n";
        return;
    }
    const char* cond = "false";
    switch (bb.branch_cond) {
        case BranchCondition::Scc0: cond = "(!scc)"; break;
        case BranchCondition::Scc1: cond = "(scc)"; break;
        case BranchCondition::VccZ: cond = "(!vcc)"; break;
        case BranchCondition::VccNz: cond = "(vcc)"; break;
        case BranchCondition::ExecZ: cond = "(!exec_active)"; break;
        case BranchCondition::ExecNz: cond = "(exec_active)"; break;
        default: break;
    }
    const std::string t_blk =
        bb.branch_target_block >= 0 ? (std::to_string(bb.branch_target_block) + "u") : "0xFFFFFFFFu";
    const std::string f_blk =
        bb.fallthrough_block >= 0 ? (std::to_string(bb.fallthrough_block) + "u") : "0xFFFFFFFFu";
    out << indent << "_pc_block = " << cond << " ? " << t_blk << " : " << f_blk << ";\n";
}

}  // namespace

Result<LiftedShader> LiftXs2ToHlsl(const std::vector<uint8_t>& xs2_bytes) {
    LiftedShader result{};
    Status st = ParseXs2Container(xs2_bytes, result.module);
    if (!st.ok()) {
        return Result<LiftedShader>::Err(std::move(st));
    }

    st = DecodeRdna2Program(result.module.code_words, result.instructions);
    if (!st.ok()) {
        return Result<LiftedShader>::Err(std::move(st));
    }

    st = BuildRdna2Cfg(result.instructions, result.cfg);
    if (!st.ok()) {
        return Result<LiftedShader>::Err(std::move(st));
    }

    result.disassembly = DisassembleRdna2(result.instructions);

    // Determine resource declarations, register bounds, and export outputs.
    uint32_t max_sgpr = std::max<uint32_t>(4u, result.module.header.sgpr_count);
    uint32_t max_vgpr = std::max<uint32_t>(8u, result.module.header.vgpr_count);
    bool uses_perm = false;
    std::set<uint8_t> exp_mrt;
    std::set<uint8_t> exp_pos;
    std::set<uint8_t> exp_param;

    for (const auto& insn : result.instructions) {
        if (insn.mnemonic == Rdna2Mnemonic::V_PERM_B32) uses_perm = true;
        max_sgpr = std::max(max_sgpr, OperandReadU32MaxReg(insn.dst, true) + 4u);
        max_sgpr = std::max(max_sgpr, OperandReadU32MaxReg(insn.src0, true) + 4u);
        max_sgpr = std::max(max_sgpr, OperandReadU32MaxReg(insn.src1, true) + 4u);
        max_sgpr = std::max(max_sgpr, OperandReadU32MaxReg(insn.src2, true) + 4u);
        max_vgpr = std::max(max_vgpr, OperandReadU32MaxReg(insn.dst, false) + 4u);
        max_vgpr = std::max(max_vgpr, OperandReadU32MaxReg(insn.src0, false) + 4u);
        max_vgpr = std::max(max_vgpr, OperandReadU32MaxReg(insn.src1, false) + 4u);
        max_vgpr = std::max(max_vgpr, OperandReadU32MaxReg(insn.src2, false) + 4u);
        max_vgpr = std::max(max_vgpr, OperandReadU32MaxReg(insn.src3, false) + 4u);

        if (insn.mnemonic == Rdna2Mnemonic::EXP) {
            if (insn.exp_target <= 7) exp_mrt.insert(insn.exp_target);
            else if (insn.exp_target >= 12 && insn.exp_target <= 15) exp_pos.insert(insn.exp_target - 12);
            else if (insn.exp_target >= 32) exp_param.insert(static_cast<uint8_t>(insn.exp_target - 32));
        }
    }
    max_sgpr = std::min<uint32_t>(max_sgpr, 106u);
    max_vgpr = std::min<uint32_t>(max_vgpr, 256u);

    uint32_t sys_sgpr_base = 0;
    for (const auto& b : result.module.bindings) {
        if (b.kind == Xs2BindingKind::Cbv || b.kind == Xs2BindingKind::Srv ||
            b.kind == Xs2BindingKind::Uav) {
            sys_sgpr_base = std::max<uint32_t>(
                sys_sgpr_base, static_cast<uint32_t>(b.sgpr_base) + b.sgpr_count);
        } else if (b.kind == Xs2BindingKind::VertexInput) {
            ++result.input_param_count;
        }
    }
    if (sys_sgpr_base + 3u > max_sgpr) {
        max_sgpr = std::min<uint32_t>(sys_sgpr_base + 3u, 106u);
    }

    std::ostringstream hlsl;
    hlsl << "// Lifted from Scarlett RDNA2 (XS2) microcode by ScarlettX\n";

    std::set<uint8_t> declared_cbv, declared_srv, declared_uav;
    for (const auto& b : result.module.bindings) {
        std::string space_suffix;
        if (b.register_space != 0) {
            space_suffix = ", space" + std::to_string(static_cast<uint32_t>(b.register_space));
        }
        if (b.kind == Xs2BindingKind::Cbv && declared_cbv.insert(b.shader_register).second) {
            hlsl << "cbuffer CBuf_b" << static_cast<uint32_t>(b.shader_register)
                 << " : register(b" << static_cast<uint32_t>(b.shader_register)
                 << space_suffix << ") {\n"
                 << "    uint cb_b" << static_cast<uint32_t>(b.shader_register) << "[1024];\n"
                 << "};\n";
        } else if (b.kind == Xs2BindingKind::Srv && declared_srv.insert(b.shader_register).second) {
            hlsl << "ByteAddressBuffer srv_t" << static_cast<uint32_t>(b.shader_register)
                 << " : register(t" << static_cast<uint32_t>(b.shader_register)
                 << space_suffix << ");\n";
        } else if (b.kind == Xs2BindingKind::Uav && declared_uav.insert(b.shader_register).second) {
            hlsl << "RWByteAddressBuffer uav_u" << static_cast<uint32_t>(b.shader_register)
                 << " : register(u" << static_cast<uint32_t>(b.shader_register)
                 << space_suffix << ");\n";
        }
    }

    if (uses_perm) {
        hlsl << "uint _rdna2_perm(uint s0, uint s1, uint sel) {\n"
             << "    uint res = 0u;\n"
             << "    [unroll] for (uint i = 0u; i < 4u; ++i) {\n"
             << "        uint idx = (sel >> (i * 8u)) & 0xFFu;\n"
             << "        uint b = 0u;\n"
             << "        if (idx < 4u) b = (s1 >> (idx * 8u)) & 0xFFu;\n"
             << "        else if (idx < 8u) b = (s0 >> ((idx - 4u) * 8u)) & 0xFFu;\n"
             << "        else if (idx == 12u) b = 0xFFu;\n"
             << "        res |= (b << (i * 8u));\n"
             << "    }\n"
             << "    return res;\n"
             << "}\n";
    }

    const Xs2Stage stage = result.module.header.stage;
    if (stage == Xs2Stage::Vertex) {
        if (exp_pos.empty()) exp_pos.insert(0);
        hlsl << "struct VSOut {\n";
        for (uint8_t p : exp_pos) {
            if (p == 0) hlsl << "    float4 pos : SV_POSITION;\n";
            else hlsl << "    float4 pos" << static_cast<uint32_t>(p) << " : POSITION" << static_cast<uint32_t>(p) << ";\n";
        }
        for (uint8_t p : exp_param) {
            hlsl << "    float4 param" << static_cast<uint32_t>(p) << " : TEXCOORD" << static_cast<uint32_t>(p) << ";\n";
        }
        hlsl << "};\n";
        result.output_param_count = static_cast<uint32_t>(exp_pos.size() + exp_param.size());
        hlsl << "VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
             << "    VSOut _out = (VSOut)0;\n";
    } else if (stage == Xs2Stage::Pixel) {
        if (exp_mrt.empty()) exp_mrt.insert(0);
        std::set<uint8_t> ps_params;
        for (const auto& b : result.module.bindings) {
            if (b.kind == Xs2BindingKind::VertexInput) {
                ps_params.insert(b.shader_register);
            }
        }
        hlsl << "struct PSIn {\n"
             << "    float4 pos : SV_POSITION;\n";
        for (uint8_t p : ps_params) {
            hlsl << "    float4 param" << static_cast<uint32_t>(p) << " : TEXCOORD" << static_cast<uint32_t>(p) << ";\n";
        }
        hlsl << "};\n";
        hlsl << "struct PSOut {\n";
        for (uint8_t m : exp_mrt) {
            hlsl << "    float4 mrt" << static_cast<uint32_t>(m) << " : SV_TARGET" << static_cast<uint32_t>(m) << ";\n";
        }
        hlsl << "};\n";
        result.output_param_count = static_cast<uint32_t>(exp_mrt.size());
        hlsl << "PSOut main(PSIn _pin) {\n"
             << "    PSOut _out = (PSOut)0;\n";
    } else {
        hlsl << "[numthreads(" << result.module.header.num_threads_x << ", "
             << result.module.header.num_threads_y << ", "
             << result.module.header.num_threads_z << ")]\n"
             << "void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint3 dtid : SV_DispatchThreadID) {\n";
    }

    hlsl << "    uint s[" << max_sgpr << "];\n"
         << "    uint v[" << max_vgpr << "];\n"
         << "    [unroll] for (uint _si = 0u; _si < " << max_sgpr << "u; ++_si) s[_si] = 0u;\n"
         << "    [unroll] for (uint _vi = 0u; _vi < " << max_vgpr << "u; ++_vi) v[_vi] = 0u;\n"
         << "    bool scc = false;\n"
         << "    bool vcc = false;\n"
         << "    bool exec_active = true;\n"
         << "    uint m0_reg = 0u;\n"
         << "    uint _discard_u32 = 0u;\n";

    if (stage == Xs2Stage::Compute) {
        hlsl << "    v[0] = gtid.x;\n"
             << "    v[1] = gtid.y;\n"
             << "    v[2] = gtid.z;\n";
        if (sys_sgpr_base + 2u < max_sgpr) {
            hlsl << "    s[" << sys_sgpr_base << "] = gid.x;\n"
                 << "    s[" << (sys_sgpr_base + 1u) << "] = gid.y;\n"
                 << "    s[" << (sys_sgpr_base + 2u) << "] = gid.z;\n";
        }
    } else if (stage == Xs2Stage::Vertex) {
        hlsl << "    v[0] = vid;\n"
             << "    v[1] = iid;\n";
    } else if (stage == Xs2Stage::Pixel) {
        uint32_t v_cursor = 0;
        for (const auto& b : result.module.bindings) {
            if (b.kind == Xs2BindingKind::VertexInput && v_cursor + 3u < max_vgpr) {
                const uint32_t reg = b.shader_register;
                hlsl << "    v[" << v_cursor << "] = asuint(_pin.param" << reg << ".x);\n"
                     << "    v[" << (v_cursor + 1u) << "] = asuint(_pin.param" << reg << ".y);\n"
                     << "    v[" << (v_cursor + 2u) << "] = asuint(_pin.param" << reg << ".z);\n"
                     << "    v[" << (v_cursor + 3u) << "] = asuint(_pin.param" << reg << ".w);\n";
                v_cursor += 4u;
            }
        }
    }

    if (result.cfg.blocks.size() == 1 && !IsBranchMnemonic(
            result.instructions[result.cfg.blocks[0].instruction_indices.back()].mnemonic)) {
        for (size_t idx : result.cfg.blocks[0].instruction_indices) {
            EmitHlslInstruction(hlsl, result.module, result.instructions[idx], "    ");
        }
    } else {
        hlsl << "    uint _pc_block = 0u;\n"
             << "    [loop]\n"
             << "    for (uint _step = 0u; _step < 4096u && _pc_block != 0xFFFFFFFFu; ++_step) {\n";
        for (size_t bi = 0; bi < result.cfg.blocks.size(); ++bi) {
            const auto& bb = result.cfg.blocks[bi];
            hlsl << "        " << (bi == 0 ? "if" : "else if") << " (_pc_block == " << bb.id << "u) {\n";
            for (size_t idx : bb.instruction_indices) {
                EmitHlslInstruction(hlsl, result.module, result.instructions[idx], "            ");
            }
            EmitBranchFooter(hlsl, bb, "            ");
            hlsl << "        }\n";
        }
        hlsl << "        else {\n"
             << "            _pc_block = 0xFFFFFFFFu;\n"
             << "        }\n"
             << "    }\n";
    }

    if (stage == Xs2Stage::Vertex || stage == Xs2Stage::Pixel) {
        hlsl << "    return _out;\n";
    }
    hlsl << "}\n";

    result.hlsl_source = hlsl.str();
    return Result<LiftedShader>::Ok(std::move(result));
}

Result<LiftedShader> LiftXs2ToDxbc(const std::vector<uint8_t>& xs2_bytes) {
    auto lifted_res = LiftXs2ToHlsl(xs2_bytes);
    if (!lifted_res.ok()) {
        return lifted_res;
    }

    bool uses_nonzero_space = false;
    for (const auto& b : lifted_res.value.module.bindings) {
        if (b.register_space != 0) {
            uses_nonzero_space = true;
            break;
        }
    }

    const char* target = nullptr;
    switch (lifted_res.value.module.header.stage) {
        case Xs2Stage::Compute: target = uses_nonzero_space ? "cs_5_1" : "cs_5_0"; break;
        case Xs2Stage::Vertex: target = uses_nonzero_space ? "vs_5_1" : "vs_5_0"; break;
        case Xs2Stage::Pixel: target = uses_nonzero_space ? "ps_5_1" : "ps_5_0"; break;
        case Xs2Stage::Geometry: target = uses_nonzero_space ? "gs_5_1" : "gs_5_0"; break;
        case Xs2Stage::Hull: target = uses_nonzero_space ? "hs_5_1" : "hs_5_0"; break;
        case Xs2Stage::Domain: target = uses_nonzero_space ? "ds_5_1" : "ds_5_0"; break;
        default:
            return Result<LiftedShader>::Err(
                StatusCode::UnsupportedFormat,
                "XS2 mesh/amplification stages require DXIL sm_6_5 compiler support");
    }

    Status st = CompileHlslToDxbc(
        lifted_res.value.hlsl_source, "main", target, lifted_res.value.dxbc_bytecode);
    if (!st.ok()) {
        return Result<LiftedShader>::Err(std::move(st));
    }
    return lifted_res;
}

}  // namespace scarlettx::gpu
