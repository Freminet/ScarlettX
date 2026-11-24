#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx::gpu {

template <typename T>
struct Result {
    Status status;
    T value{};

    bool ok() const noexcept { return status.ok(); }
    explicit operator bool() const noexcept { return ok(); }

    static Result<T> Ok(T v) { return {Status::Ok(), std::move(v)}; }
    static Result<T> Err(StatusCode code, std::string msg) {
        return {Status::Error(code, std::move(msg)), {}};
    }
    static Result<T> Err(Status st) { return {std::move(st), {}}; }
};

// ---------------------------------------------------------------------------
// Clean-room XS2\0 Shader Container Format
// ---------------------------------------------------------------------------
enum class Xs2Stage : uint16_t {
    Compute = 0,
    Vertex = 1,
    Pixel = 2,
    Geometry = 3,
    Hull = 4,
    Domain = 5,
    Mesh = 6,
    Amplification = 7,
};

enum class Xs2WaveMode : uint8_t {
    Wave32 = 32,
    Wave64 = 64,
};

enum class Xs2BindingKind : uint8_t {
    Cbv = 0,
    Srv = 1,
    Uav = 2,
    Sampler = 3,
    VertexInput = 4,
    RenderTarget = 5,
};

struct Xs2ResourceBinding {
    Xs2BindingKind kind = Xs2BindingKind::Srv;
    uint8_t shader_register = 0;
    uint8_t register_space = 0;
    uint8_t sgpr_base = 0;
    uint8_t sgpr_count = 4;
    uint8_t format = 0;
    uint16_t reserved = 0;
    uint32_t byte_stride = 4;
    uint32_t element_count = 0;
};

struct Xs2Header {
    uint16_t version = 1;
    Xs2Stage stage = Xs2Stage::Compute;
    Xs2WaveMode wave_mode = Xs2WaveMode::Wave32;
    uint8_t sgpr_count = 16;
    uint16_t vgpr_count = 8;
    uint16_t num_threads_x = 64;
    uint16_t num_threads_y = 1;
    uint16_t num_threads_z = 1;
    uint16_t binding_count = 0;
    uint32_t code_offset = 0;
    uint32_t code_size = 0;
    uint32_t binding_offset = 0;
};

struct Xs2Module {
    Xs2Header header{};
    std::vector<Xs2ResourceBinding> bindings;
    std::vector<uint32_t> code_words;
    std::vector<uint8_t> raw_code_bytes;
};

constexpr size_t kXs2HeaderSize = 32;
constexpr size_t kXs2BindingSize = 16;

Status ParseXs2Container(const std::vector<uint8_t>& bytes, Xs2Module& out_module);
std::vector<uint8_t> BuildXs2Container(
    const Xs2Header& header,
    const std::vector<Xs2ResourceBinding>& bindings,
    const std::vector<uint32_t>& code_words);

// ---------------------------------------------------------------------------
// AMD RDNA2 (GFX10.3) Instruction Representation
// ---------------------------------------------------------------------------
enum class Rdna2Encoding : uint8_t {
    Unknown = 0,
    Sop2,
    Sop1,
    Sopk,
    Sopc,
    Sopp,
    Vop2,
    Vop1,
    Vopc,
    Vop3,
    Smem,
    Mubuf,
    Flat,
    Exp,
};

enum class Rdna2OperandKind : uint8_t {
    None = 0,
    Sgpr,
    Vgpr,
    VccLo,
    VccHi,
    ExecLo,
    ExecHi,
    M0,
    Scc,
    Null,
    InlineInt,
    InlineFloat,
    Literal32,
};

struct Rdna2Operand {
    Rdna2OperandKind kind = Rdna2OperandKind::None;
    uint16_t reg_index = 0;
    int32_t imm_i32 = 0;
    float imm_f32 = 0.0f;
    uint32_t literal_u32 = 0;
    bool neg = false;
    bool abs = false;

    static Rdna2Operand Sgpr(uint16_t idx) {
        Rdna2Operand op{};
        op.kind = Rdna2OperandKind::Sgpr;
        op.reg_index = idx;
        return op;
    }
    static Rdna2Operand Vgpr(uint16_t idx) {
        Rdna2Operand op{};
        op.kind = Rdna2OperandKind::Vgpr;
        op.reg_index = idx;
        return op;
    }
};

enum class Rdna2Mnemonic : uint16_t {
    Unknown = 0,
    // SOP2
    S_ADD_U32,
    S_SUB_U32,
    S_ADD_I32,
    S_SUB_I32,
    S_MIN_I32,
    S_MIN_U32,
    S_MAX_I32,
    S_MAX_U32,
    S_AND_B32,
    S_OR_B32,
    S_XOR_B32,
    S_LSHL_B32,
    S_LSHR_B32,
    S_ASHR_I32,
    S_MUL_I32,
    // SOP1
    S_MOV_B32,
    S_MOV_B64,
    S_NOT_B32,
    S_WQM_B32,
    S_GETPC_B64,
    // SOPK
    S_MOVK_I32,
    S_CMOVK_I32,
    S_CMPK_EQ_I32,
    S_CMPK_LG_I32,
    S_CMPK_GT_I32,
    S_CMPK_GE_I32,
    S_CMPK_LT_I32,
    S_CMPK_LE_I32,
    S_ADDK_I32,
    S_MULK_I32,
    // SOPC
    S_CMP_EQ_I32,
    S_CMP_LG_I32,
    S_CMP_GT_I32,
    S_CMP_GE_I32,
    S_CMP_LT_I32,
    S_CMP_LE_I32,
    S_CMP_EQ_U32,
    S_CMP_LG_U32,
    S_CMP_GT_U32,
    S_CMP_GE_U32,
    S_CMP_LT_U32,
    S_CMP_LE_U32,
    // SOPP
    S_NOP,
    S_ENDPGM,
    S_BRANCH,
    S_CBRANCH_SCC0,
    S_CBRANCH_SCC1,
    S_CBRANCH_VCCZ,
    S_CBRANCH_VCCNZ,
    S_CBRANCH_EXECZ,
    S_CBRANCH_EXECNZ,
    S_BARRIER,
    S_WAITCNT,
    // VOP2
    V_CNDMASK_B32,
    V_ADD_F32,
    V_SUB_F32,
    V_MUL_F32,
    V_FMAC_F32,
    V_ADD_NC_U32,
    V_SUB_NC_U32,
    V_AND_B32,
    V_OR_B32,
    V_XOR_B32,
    V_LSHLREV_B32,
    V_LSHRREV_B32,
    V_MIN_F32,
    V_MAX_F32,
    // VOP1
    V_NOP,
    V_MOV_B32,
    V_CVT_F32_I32,
    V_CVT_F32_U32,
    V_CVT_I32_F32,
    V_RCP_F32,
    V_RSQ_F32,
    V_SQRT_F32,
    V_SIN_F32,
    V_COS_F32,
    V_FLOOR_F32,
    V_FRACT_F32,
    // VOPC
    V_CMP_LT_F32,
    V_CMP_EQ_F32,
    V_CMP_LE_F32,
    V_CMP_GT_F32,
    V_CMP_NEQ_F32,
    V_CMP_GE_F32,
    V_CMP_LT_U32,
    V_CMP_EQ_U32,
    V_CMP_GT_U32,
    // VOP3
    V_MAD_F32,
    V_MAD_U32_U24,
    V_BFE_U32,
    V_BFI_B32,
    V_FMA_F32,
    V_PERM_B32,
    // SMEM
    S_LOAD_DWORD,
    S_LOAD_DWORDX2,
    S_LOAD_DWORDX4,
    S_BUFFER_LOAD_DWORD,
    S_BUFFER_LOAD_DWORDX2,
    S_BUFFER_LOAD_DWORDX4,
    // MUBUF
    BUFFER_LOAD_DWORD,
    BUFFER_LOAD_DWORDX2,
    BUFFER_LOAD_DWORDX4,
    BUFFER_STORE_DWORD,
    BUFFER_STORE_DWORDX2,
    BUFFER_STORE_DWORDX4,
    // FLAT / GLOBAL
    GLOBAL_LOAD_DWORD,
    GLOBAL_LOAD_DWORDX2,
    GLOBAL_LOAD_DWORDX4,
    GLOBAL_STORE_DWORD,
    GLOBAL_STORE_DWORDX2,
    GLOBAL_STORE_DWORDX4,
    // EXP
    EXP,
};

struct Rdna2Instruction {
    uint32_t byte_offset = 0;
    uint32_t size_bytes = 4;
    uint32_t word0 = 0;
    uint32_t word1 = 0;
    uint32_t literal = 0;
    bool has_literal = false;

    Rdna2Encoding encoding = Rdna2Encoding::Unknown;
    Rdna2Mnemonic mnemonic = Rdna2Mnemonic::Unknown;
    uint16_t raw_opcode = 0;

    Rdna2Operand dst{};
    Rdna2Operand src0{};
    Rdna2Operand src1{};
    Rdna2Operand src2{};
    Rdna2Operand src3{};

    int16_t simm16 = 0;
    uint32_t offset = 0;
    uint8_t flat_seg = 0;
    bool offen = false;
    bool idxen = false;
    bool glc = false;
    bool slc = false;
    bool dlc = false;
    bool clamp = false;
    uint8_t omod = 0;

    uint8_t exp_target = 0;
    uint8_t exp_en = 0;
    bool exp_done = false;
    bool exp_compr = false;
    bool exp_vm = false;
};

const char* MnemonicName(Rdna2Mnemonic m) noexcept;

Status DecodeRdna2Instruction(
    const uint32_t* words,
    size_t word_count,
    size_t word_index,
    Rdna2Instruction& out_insn);

Status DecodeRdna2Program(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_insns);

std::string FormatRdna2Instruction(const Rdna2Instruction& insn);
std::string DisassembleRdna2(const std::vector<Rdna2Instruction>& insns);

// ---------------------------------------------------------------------------
// Control-Flow Graph (CFG) & HLSL/DXBC Lifting
// ---------------------------------------------------------------------------
enum class BranchCondition : uint8_t {
    None = 0,
    Unconditional,
    Scc0,
    Scc1,
    VccZ,
    VccNz,
    ExecZ,
    ExecNz,
};

struct Rdna2BasicBlock {
    uint32_t id = 0;
    uint32_t start_byte = 0;
    uint32_t end_byte = 0;
    std::vector<size_t> instruction_indices;
    int32_t fallthrough_block = -1;
    int32_t branch_target_block = -1;
    BranchCondition branch_cond = BranchCondition::None;
    bool terminates = false;
};

struct Rdna2Cfg {
    std::vector<Rdna2BasicBlock> blocks;
};

Status BuildRdna2Cfg(const std::vector<Rdna2Instruction>& insns, Rdna2Cfg& out_cfg);

struct LiftedShader {
    Xs2Module module;
    std::vector<Rdna2Instruction> instructions;
    Rdna2Cfg cfg;
    std::string hlsl_source;
    std::string disassembly;
    std::vector<uint8_t> dxbc_bytecode;
    uint32_t input_param_count = 0;
    uint32_t output_param_count = 0;
};

Result<LiftedShader> LiftXs2ToHlsl(const std::vector<uint8_t>& xs2_bytes);
Result<LiftedShader> LiftXs2ToDxbc(const std::vector<uint8_t>& xs2_bytes);

// ---------------------------------------------------------------------------
// Clean-room GFX10.3 Machine-Word Encoders (for synthetic test construction)
// ---------------------------------------------------------------------------
namespace enc {

constexpr uint16_t kSrcVccLo = 106;
constexpr uint16_t kSrcVccHi = 107;
constexpr uint16_t kSrcM0 = 124;
constexpr uint16_t kSrcNull = 125;
constexpr uint16_t kSrcExecLo = 126;
constexpr uint16_t kSrcExecHi = 127;
constexpr uint16_t kSrcInlineZero = 128;
constexpr uint16_t kSrcLiteral = 255;

constexpr uint16_t InlineInt(int32_t val) {
    if (val >= 0 && val <= 64) return static_cast<uint16_t>(128 + val);
    return static_cast<uint16_t>(192 - val);  // -1..-16 -> 193..208
}
constexpr uint16_t VgprSrc(uint8_t v) { return static_cast<uint16_t>(256u + v); }

inline uint32_t Sop2(uint8_t op, uint8_t sdst, uint8_t ssrc1, uint8_t ssrc0) {
    return (0x2u << 30) | ((static_cast<uint32_t>(op) & 0x7Fu) << 23) |
           ((static_cast<uint32_t>(sdst) & 0x7Fu) << 16) |
           (static_cast<uint32_t>(ssrc1) << 8) | static_cast<uint32_t>(ssrc0);
}

inline uint32_t Sop1(uint8_t op, uint8_t sdst, uint8_t ssrc0) {
    return (0x17Du << 23) | ((static_cast<uint32_t>(sdst) & 0x7Fu) << 16) |
           (static_cast<uint32_t>(op) << 8) | static_cast<uint32_t>(ssrc0);
}

inline uint32_t Sopk(uint8_t op, uint8_t sdst, int16_t simm16) {
    return (0xBu << 28) | ((static_cast<uint32_t>(op) & 0x1Fu) << 23) |
           ((static_cast<uint32_t>(sdst) & 0x7Fu) << 16) | static_cast<uint16_t>(simm16);
}

inline uint32_t Sopc(uint8_t op, uint8_t ssrc1, uint8_t ssrc0) {
    return (0x17Eu << 23) | ((static_cast<uint32_t>(op) & 0x7Fu) << 16) |
           (static_cast<uint32_t>(ssrc1) << 8) | static_cast<uint32_t>(ssrc0);
}

inline uint32_t Sopp(uint8_t op, int16_t simm16 = 0) {
    return (0x17Fu << 23) | ((static_cast<uint32_t>(op) & 0x7Fu) << 16) |
           static_cast<uint16_t>(simm16);
}

inline uint32_t Vop2(uint8_t op, uint8_t vdst, uint8_t vsrc1, uint16_t src0) {
    return ((static_cast<uint32_t>(op) & 0x3Fu) << 25) |
           (static_cast<uint32_t>(vdst) << 17) |
           (static_cast<uint32_t>(vsrc1) << 9) | (static_cast<uint32_t>(src0) & 0x1FFu);
}

inline uint32_t Vop1(uint8_t op, uint8_t vdst, uint16_t src0) {
    return (0x3Fu << 25) | (static_cast<uint32_t>(vdst) << 17) |
           (static_cast<uint32_t>(op) << 9) | (static_cast<uint32_t>(src0) & 0x1FFu);
}

inline uint32_t Vopc(uint8_t op, uint8_t vsrc1, uint16_t src0) {
    return (0x3Eu << 25) | (static_cast<uint32_t>(op) << 17) |
           (static_cast<uint32_t>(vsrc1) << 9) | (static_cast<uint32_t>(src0) & 0x1FFu);
}

inline std::pair<uint32_t, uint32_t> Vop3(
    uint16_t op,
    uint8_t vdst,
    uint16_t src0,
    uint16_t src1,
    uint16_t src2,
    uint8_t neg = 0,
    uint8_t abs = 0,
    bool clamp = false,
    uint8_t omod = 0) {
    const uint32_t w0 = (0x35u << 26) | ((static_cast<uint32_t>(op) & 0x3FFu) << 16) |
                        (clamp ? (1u << 15) : 0u) | ((static_cast<uint32_t>(abs) & 0x7u) << 8) |
                        static_cast<uint32_t>(vdst);
    const uint32_t w1 = ((static_cast<uint32_t>(neg) & 0x7u) << 29) |
                        ((static_cast<uint32_t>(omod) & 0x3u) << 27) |
                        ((static_cast<uint32_t>(src2) & 0x1FFu) << 18) |
                        ((static_cast<uint32_t>(src1) & 0x1FFu) << 9) |
                        (static_cast<uint32_t>(src0) & 0x1FFu);
    return {w0, w1};
}

inline std::pair<uint32_t, uint32_t> Smem(
    uint8_t op,
    uint8_t sdst,
    uint8_t sbase_pair,
    uint8_t soffset,
    uint32_t offset20,
    bool glc = false,
    bool dlc = false) {
    const uint32_t w0 = (0x3Du << 26) | (static_cast<uint32_t>(op) << 18) |
                        (glc ? (1u << 16) : 0u) | (dlc ? (1u << 14) : 0u) |
                        ((static_cast<uint32_t>(sdst) & 0x7Fu) << 6) |
                        (static_cast<uint32_t>(sbase_pair) & 0x3Fu);
    const uint32_t w1 = ((static_cast<uint32_t>(soffset) & 0x7Fu) << 25) |
                        (offset20 & 0x1FFFFFu);
    return {w0, w1};
}

inline std::pair<uint32_t, uint32_t> Mubuf(
    uint8_t op,
    uint8_t vdata,
    uint8_t vaddr,
    uint8_t srsrc_quad,
    uint8_t soffset,
    uint16_t offset12 = 0,
    bool offen = false,
    bool idxen = false,
    bool glc = false,
    bool slc = false) {
    const uint32_t w0 = (0x38u << 26) | ((static_cast<uint32_t>(op) & 0x7Fu) << 18) |
                        (slc ? (1u << 17) : 0u) | (glc ? (1u << 14) : 0u) |
                        (idxen ? (1u << 13) : 0u) | (offen ? (1u << 12) : 0u) |
                        (static_cast<uint32_t>(offset12) & 0xFFFu);
    const uint32_t w1 = (static_cast<uint32_t>(soffset) << 24) |
                        ((static_cast<uint32_t>(srsrc_quad) & 0x1Fu) << 16) |
                        (static_cast<uint32_t>(vdata) << 8) | static_cast<uint32_t>(vaddr);
    return {w0, w1};
}

inline std::pair<uint32_t, uint32_t> FlatGlobal(
    uint8_t op,
    uint8_t vdst_or_vdata,
    uint8_t vaddr,
    uint8_t saddr,
    uint8_t vdata_store = 0,
    uint16_t offset12 = 0,
    bool glc = false,
    bool slc = false) {
    const uint32_t w0 = (0x37u << 26) | ((static_cast<uint32_t>(op) & 0x7Fu) << 18) |
                        (slc ? (1u << 17) : 0u) | (glc ? (1u << 16) : 0u) |
                        (2u << 14) |  // seg = 2 (GLOBAL)
                        (static_cast<uint32_t>(offset12) & 0xFFFu);
    const uint32_t w1 = (static_cast<uint32_t>(vdst_or_vdata) << 24) |
                        ((static_cast<uint32_t>(saddr) & 0x7Fu) << 16) |
                        (static_cast<uint32_t>(vdata_store) << 8) | static_cast<uint32_t>(vaddr);
    return {w0, w1};
}

inline std::pair<uint32_t, uint32_t> Exp(
    uint8_t target,
    uint8_t en,
    uint8_t vsrc0,
    uint8_t vsrc1,
    uint8_t vsrc2,
    uint8_t vsrc3,
    bool done = false,
    bool vm = false,
    bool compr = false) {
    const uint32_t w0 = (0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u) |
                        (compr ? (1u << 10) : 0u) | ((static_cast<uint32_t>(target) & 0x3Fu) << 4) |
                        (static_cast<uint32_t>(en) & 0xFu);
    const uint32_t w1 = (static_cast<uint32_t>(vsrc3) << 24) |
                        (static_cast<uint32_t>(vsrc2) << 16) |
                        (static_cast<uint32_t>(vsrc1) << 8) | static_cast<uint32_t>(vsrc0);
    return {w0, w1};
}

}  // namespace enc

}  // namespace scarlettx::gpu
