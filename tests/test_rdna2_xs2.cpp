#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/rdna2_xs2.h"
#include "scarlettx/gpu/shader_ir.h"
#include "sx_check.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#include <windows.h>
#endif

namespace {

using namespace scarlettx;
using namespace scarlettx::gpu;

uint32_t FloatBits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

void EmitPair(std::vector<uint32_t>& words, std::pair<uint32_t, uint32_t> p) {
    words.push_back(p.first);
    words.push_back(p.second);
}

// Test 1: Decode and disassemble all GFX10.3 instruction families including 32-bit literals.
bool RunTest1DecoderAndDisassembler() {
    std::vector<uint32_t> words;
    // SOP1: s_mov_b32 s2, 0x12345678 (with trailing literal word)
    words.push_back(enc::Sop1(0x03, 2, enc::kSrcLiteral));
    words.push_back(0x12345678u);
    // SOP2: s_add_u32 s3, s2, 4
    words.push_back(enc::Sop2(0x00, 3, enc::InlineInt(4), 2));
    // SOPK: s_movk_i32 s4, -7
    words.push_back(enc::Sopk(0x00, 4, -7));
    // SOPC: s_cmp_lt_u32 s3, s4
    words.push_back(enc::Sopc(0x0A, 4, 3));
    // SOPP: s_cbranch_scc1 +2
    words.push_back(enc::Sopp(0x05, 2));
    // VOP1: v_mov_b32 v1, 2.5f (literal)
    words.push_back(enc::Vop1(0x01, 1, enc::kSrcLiteral));
    words.push_back(FloatBits(2.5f));
    // VOP2: v_add_f32 v2, 1.0f, v1
    words.push_back(enc::Vop2(0x03, 2, 1, 242));
    // VOPC: v_cmp_gt_f32 vcc, v2, v1
    words.push_back(enc::Vopc(0x04, 1, enc::VgprSrc(2)));
    // VOP3: v_fma_f32 v3, v1, v2, 0.5f
    EmitPair(words, enc::Vop3(0x1CB, 3, enc::VgprSrc(1), enc::VgprSrc(2), 240));
    // SMEM: s_load_dwordx4 s[8:11], s[0:1], null offset:16
    EmitPair(words, enc::Smem(0x02, 8, 0, enc::kSrcNull, 16));
    // MUBUF: buffer_load_dword v4, v0, s[0:3], 0 offen offset:8
    EmitPair(words, enc::Mubuf(0x0C, 4, 0, 0, enc::kSrcInlineZero, 8, true));
    // MUBUF: buffer_store_dword v3, v0, s[4:7], 0 offen
    EmitPair(words, enc::Mubuf(0x1C, 3, 0, 1, enc::kSrcInlineZero, 0, true));
    // FLAT/GLOBAL: global_load_dword v5, v0, null offset:4
    EmitPair(words, enc::FlatGlobal(0x0C, 5, 0, enc::kSrcNull, 0, 4));
    // EXP: exp mrt0, v0, v1, v2, v3 done vm
    EmitPair(words, enc::Exp(0, 0xF, 0, 1, 2, 3, true, true));
    // SOPP: s_endpgm
    words.push_back(enc::Sopp(0x01, 0));

    std::vector<Rdna2Instruction> insns;
    SX_REQUIRE(DecodeRdna2Program(words, insns).ok());
    SX_REQUIRE(insns.size() == 15);
    SX_REQUIRE(insns[0].mnemonic == Rdna2Mnemonic::S_MOV_B32);
    SX_REQUIRE(insns[0].has_literal && insns[0].literal == 0x12345678u);
    SX_REQUIRE(insns[1].mnemonic == Rdna2Mnemonic::S_ADD_U32);
    SX_REQUIRE(insns[2].mnemonic == Rdna2Mnemonic::S_MOVK_I32 && insns[2].simm16 == -7);
    SX_REQUIRE(insns[3].mnemonic == Rdna2Mnemonic::S_CMP_LT_U32);
    SX_REQUIRE(insns[4].mnemonic == Rdna2Mnemonic::S_CBRANCH_SCC1 && insns[4].simm16 == 2);
    SX_REQUIRE(insns[5].mnemonic == Rdna2Mnemonic::V_MOV_B32 && insns[5].size_bytes == 8);
    SX_REQUIRE(insns[6].mnemonic == Rdna2Mnemonic::V_ADD_F32);
    SX_REQUIRE(insns[7].mnemonic == Rdna2Mnemonic::V_CMP_GT_F32);
    SX_REQUIRE(insns[8].mnemonic == Rdna2Mnemonic::V_FMA_F32);
    SX_REQUIRE(insns[9].mnemonic == Rdna2Mnemonic::S_LOAD_DWORDX4 && insns[9].offset == 16);
    SX_REQUIRE(insns[10].mnemonic == Rdna2Mnemonic::BUFFER_LOAD_DWORD && insns[10].offen);
    SX_REQUIRE(insns[11].mnemonic == Rdna2Mnemonic::BUFFER_STORE_DWORD && insns[11].src1.reg_index == 4);
    SX_REQUIRE(insns[12].mnemonic == Rdna2Mnemonic::GLOBAL_LOAD_DWORD && insns[12].offset == 4);
    SX_REQUIRE(insns[13].mnemonic == Rdna2Mnemonic::EXP && insns[13].exp_done && insns[13].exp_vm);
    SX_REQUIRE(insns[14].mnemonic == Rdna2Mnemonic::S_ENDPGM);

    const std::string disasm = DisassembleRdna2(insns);
    SX_REQUIRE(disasm.find("s_mov_b32 s2, 0x12345678") != std::string::npos);
    SX_REQUIRE(disasm.find("v_fma_f32 v3, v1, v2, 0.5") != std::string::npos);
    SX_REQUIRE(disasm.find("exp mrt0, v0, v1, v2, v3 done vm") != std::string::npos);
    return true;
}

// Test 2: CFG reconstruction & compilation with conditional branching and CNDMASK.
bool RunTest2ControlFlowAndBranching() {
    std::vector<uint32_t> words;
    // Block 0: compare s0 < 4, branch to block 2 if SCC==1; else fall through to block 1
    words.push_back(enc::Sopk(0x00, 0, 2));                             // 0x00: s_movk_i32 s0, 2
    words.push_back(enc::Sopc(0x0A, enc::InlineInt(4), 0));             // 0x04: s_cmp_lt_u32 s0, 4
    words.push_back(enc::Sopp(0x05, 3));                                // 0x08: s_cbranch_scc1 +3 -> 0x18
    // Block 1 (fallthrough):
    words.push_back(enc::Vop1(0x01, 1, 243));                           // 0x0C: v_mov_b32 v1, -1.0
    words.push_back(enc::Sopp(0x02, 2));                                // 0x10: s_branch +2 -> 0x1C
    words.push_back(enc::Sopp(0x00, 0));                                // 0x14: s_nop 0 (padding)
    // Block 2 (taken):
    words.push_back(enc::Vop1(0x01, 1, 242));                           // 0x18: v_mov_b32 v1, 1.0
    // Block 3 (join):
    words.push_back(enc::Vop1(0x01, 2, 244));                           // 0x1C: v_mov_b32 v2, 2.0
    words.push_back(enc::Vop2(0x04, 1, 2, 242));                        // 0x20: v_sub_f32 v1, 1.0, v2
    words.push_back(enc::Vopc(0x04, 2, enc::VgprSrc(1)));               // 0x24: v_cmp_gt_f32 vcc, v1, v2
    words.push_back(enc::Vop2(0x01, 3, 2, enc::VgprSrc(1)));            // 0x28: v_cndmask_b32 v3, v1, v2
    words.push_back(enc::Vop2(0x18, 4, 0, enc::InlineInt(2)));          // 0x2C: v_lshlrev_b32 v4, 2, v0
    EmitPair(words, enc::Mubuf(0x1C, 3, 4, 0, enc::kSrcInlineZero, 0, true));  // buffer_store_dword v3, v4, s[0:3], 0 offen
    words.push_back(enc::Sopp(0x01, 0));                                // s_endpgm

    Xs2Header hdr{};
    hdr.stage = Xs2Stage::Compute;
    hdr.wave_mode = Xs2WaveMode::Wave32;
    hdr.sgpr_count = 8;
    hdr.vgpr_count = 8;
    hdr.num_threads_x = 16;
    hdr.num_threads_y = 1;
    hdr.num_threads_z = 1;

    std::vector<Xs2ResourceBinding> bindings = {
        {Xs2BindingKind::Uav, 0, 0, 0, 4, 0, 0, 4, 16},
    };
    const std::vector<uint8_t> xs2 = BuildXs2Container(hdr, bindings, words);

    ShaderTranslationResult tr{};
    Status st = TranslateXboxShaderToPc(xs2, tr);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(!tr.pc_bytecode.empty());
    SX_REQUIRE(tr.lifted_hlsl.find("_pc_block ==") != std::string::npos);

    auto hlsl_res = LiftXs2ToHlsl(xs2);
    SX_REQUIRE(hlsl_res.ok());
    SX_REQUIRE(hlsl_res.value.cfg.blocks.size() >= 4);
    return true;
}

// Test 3: Live D3D12 Compute Execution on WARP (out[i] = a[i] * b[i] + 2.5f).
bool RunTest3LiveD3D12ComputeWarp() {
    std::vector<uint32_t> words;
    // v0 = tid.x (SV_GroupThreadID.x)
    // v1 = v0 << 2 (byte offset)
    words.push_back(enc::Vop2(0x18, 1, 0, enc::InlineInt(2)));
    // buffer_load_dword v2, v1, s[0:3], 0 offen  (t0: a[i])
    EmitPair(words, enc::Mubuf(0x0C, 2, 1, 0, enc::kSrcInlineZero, 0, true));
    // buffer_load_dword v3, v1, s[4:7], 0 offen  (t1: b[i])
    EmitPair(words, enc::Mubuf(0x0C, 3, 1, 1, enc::kSrcInlineZero, 0, true));
    // s_waitcnt 0
    words.push_back(enc::Sopp(0x0C, 0));
    // v_mul_f32 v4, v2, v3
    words.push_back(enc::Vop2(0x08, 4, 3, enc::VgprSrc(2)));
    // v_add_f32 v4, 2.5f (lit), v4
    words.push_back(enc::Vop2(0x03, 4, 4, enc::kSrcLiteral));
    words.push_back(FloatBits(2.5f));
    // buffer_store_dword v4, v1, s[8:11], 0 offen (u0: out[i])
    EmitPair(words, enc::Mubuf(0x1C, 4, 1, 2, enc::kSrcInlineZero, 0, true));
    words.push_back(enc::Sopp(0x01, 0));

    Xs2Header hdr{};
    hdr.stage = Xs2Stage::Compute;
    hdr.wave_mode = Xs2WaveMode::Wave32;
    hdr.sgpr_count = 16;
    hdr.vgpr_count = 8;
    hdr.num_threads_x = 16;
    hdr.num_threads_y = 1;
    hdr.num_threads_z = 1;

    std::vector<Xs2ResourceBinding> bindings = {
        {Xs2BindingKind::Srv, 0, 0, 0, 4, 0, 0, 4, 16},
        {Xs2BindingKind::Srv, 1, 0, 4, 4, 0, 0, 4, 16},
        {Xs2BindingKind::Uav, 0, 0, 8, 4, 0, 0, 4, 16},
    };
    const std::vector<uint8_t> xs2 = BuildXs2Container(hdr, bindings, words);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.PreferWarp = 1;
    uint64_t dev_handle = 0;
    SX_REQUIRE(D3D12XboxCreateSyntheticDevice(&dev_params, &dev_handle) == 0);

    D3D12XBOX_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumSrvs = 2;
    rs_desc.NumUavs = 1;
    uint64_t rs_handle = 0;
    SX_REQUIRE(D3D12XboxCreateRootSignature(dev_handle, &rs_desc, &rs_handle) == 0);

    D3D12XBOX_COMPUTE_PSO_DESC pso_desc{};
    pso_desc.RootSignatureHandle = rs_handle;
    pso_desc.CS = {xs2.data(), xs2.size()};
    uint64_t pso_handle = 0;
    SX_REQUIRE(D3D12XboxCreateComputePipelineState(dev_handle, &pso_desc, &pso_handle) == 0);
    SX_REQUIRE(pso_handle != 0);

#if defined(_WIN32)
    // Lift directly to verify bytecode and dispatch on the native WARP ID3D12Device with 2 SRVs + 1 UAV.
    ShaderTranslationResult tr{};
    SX_REQUIRE(TranslateXboxShaderToPc(xs2, tr).ok());

    void* native_ptr = nullptr;
    SX_REQUIRE(D3D12XboxGetNativeDevice(dev_handle, &native_ptr) == 0 && native_ptr != nullptr);
    auto* dev = static_cast<ID3D12Device*>(native_ptr);

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 1;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC native_rs_desc{};
    native_rs_desc.NumParameters = 3;
    native_rs_desc.pParameters = params;

    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    SX_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&native_rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err_blob)));
    if (err_blob) err_blob->Release();

    ID3D12RootSignature* native_rs = nullptr;
    SX_REQUIRE(SUCCEEDED(dev->CreateRootSignature(
        0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&native_rs))));
    rs_blob->Release();

    D3D12_COMPUTE_PIPELINE_STATE_DESC cp_desc{};
    cp_desc.pRootSignature = native_rs;
    cp_desc.CS = {tr.pc_bytecode.data(), tr.pc_bytecode.size()};
    ID3D12PipelineState* native_pso = nullptr;
    SX_REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(
        &cp_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&native_pso))));

    constexpr uint32_t kCount = 16;
    constexpr uint64_t kBufBytes = 256;
    float a_in[kCount]{};
    float b_in[kCount]{};
    for (uint32_t i = 0; i < kCount; ++i) {
        a_in[i] = static_cast<float>(i + 1) * 1.25f;
        b_in[i] = static_cast<float>(i) * 0.5f - 1.0f;
    }

    auto create_buf = [&](D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags,
                          D3D12_RESOURCE_STATES state) -> ID3D12Resource* {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kBufBytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ID3D12Resource* res = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                __uuidof(ID3D12Resource), reinterpret_cast<void**>(&res)))) {
            return nullptr;
        }
        return res;
    };

    ID3D12Resource* srv_a = create_buf(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* srv_b = create_buf(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* uav_out = create_buf(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* rb_out = create_buf(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    SX_REQUIRE(srv_a && srv_b && uav_out && rb_out);

    auto upload = [&](ID3D12Resource* buf, const float* src) {
        void* p = nullptr;
        D3D12_RANGE r{0, 0};
        SX_REQUIRE(SUCCEEDED(buf->Map(0, &r, &p)));
        std::memcpy(p, src, sizeof(float) * kCount);
        buf->Unmap(0, nullptr);
        return true;
    };
    SX_REQUIRE(upload(srv_a, a_in));
    SX_REQUIRE(upload(srv_b, b_in));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Fence* fence = nullptr;
    SX_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))));
    SX_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc))));
    SX_REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, native_pso, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd))));

    cmd->SetComputeRootSignature(native_rs);
    cmd->SetComputeRootShaderResourceView(0, srv_a->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(1, srv_b->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, uav_out->GetGPUVirtualAddress());
    cmd->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = uav_out;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
    cmd->CopyBufferRegion(rb_out, 0, uav_out, 0, sizeof(float) * kCount);
    SX_REQUIRE(SUCCEEDED(cmd->Close()));

    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    SX_REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence))));
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    SX_REQUIRE(ev != nullptr);
    SX_REQUIRE(SUCCEEDED(queue->Signal(fence, 1)));
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }
    CloseHandle(ev);

    float out_vals[kCount]{};
    void* mapped = nullptr;
    D3D12_RANGE rr{0, sizeof(out_vals)};
    SX_REQUIRE(SUCCEEDED(rb_out->Map(0, &rr, &mapped)));
    std::memcpy(out_vals, mapped, sizeof(out_vals));
    D3D12_RANGE empty_r{0, 0};
    rb_out->Unmap(0, &empty_r);

    for (uint32_t i = 0; i < kCount; ++i) {
        const float expected = a_in[i] * b_in[i] + 2.5f;
        SX_REQUIRE(std::fabs(out_vals[i] - expected) < 1e-5f);
    }

    fence->Release();
    cmd->Release();
    alloc->Release();
    queue->Release();
    rb_out->Release();
    uav_out->Release();
    srv_b->Release();
    srv_a->Release();
    native_pso->Release();
    native_rs->Release();
#endif

    SX_REQUIRE(D3D12XboxDestroyPipelineState(dev_handle, pso_handle) == 0);
    SX_REQUIRE(D3D12XboxDestroyRootSignature(dev_handle, rs_handle) == 0);
    SX_REQUIRE(D3D12XboxDestroyDevice(dev_handle) == 0);
    return true;
}

// Test 4: Live D3D12 Graphics VS/PS Execution via EXP POS0 & EXP MRT0.
bool RunTest4LiveD3D12GraphicsWarp() {
    // Vertex shader: full-screen triangle from vid in v0
    std::vector<uint32_t> vs_words;
    vs_words.push_back(enc::Vop1(0x01, 1, 243));                     // v1 = -1.0f
    vs_words.push_back(enc::Vop1(0x01, 2, 243));                     // v2 = -1.0f
    vs_words.push_back(enc::Vop1(0x01, 3, enc::kSrcInlineZero));     // v3 = 0.0f (z)
    vs_words.push_back(enc::Vop1(0x01, 4, 242));                     // v4 = 1.0f (w)
    vs_words.push_back(enc::Vop1(0x01, 5, enc::kSrcLiteral));        // v5 = 3.0f
    vs_words.push_back(FloatBits(3.0f));
    vs_words.push_back(enc::Vopc(0xC2, 0, enc::InlineInt(1)));       // v_cmp_eq_u32 vcc, 1, v0
    vs_words.push_back(enc::Vop2(0x01, 2, 5, enc::VgprSrc(2)));      // v_cndmask_b32 v2, v2, v5
    vs_words.push_back(enc::Vopc(0xC2, 0, enc::InlineInt(2)));       // v_cmp_eq_u32 vcc, 2, v0
    vs_words.push_back(enc::Vop2(0x01, 1, 5, enc::VgprSrc(1)));      // v_cndmask_b32 v1, v1, v5
    EmitPair(vs_words, enc::Exp(12, 0xF, 1, 2, 3, 4, true, false));  // exp pos0, v1, v2, v3, v4 done
    vs_words.push_back(enc::Sopp(0x01, 0));

    Xs2Header vs_hdr{};
    vs_hdr.stage = Xs2Stage::Vertex;
    vs_hdr.wave_mode = Xs2WaveMode::Wave32;
    vs_hdr.sgpr_count = 4;
    vs_hdr.vgpr_count = 8;
    const std::vector<uint8_t> vs_xs2 = BuildXs2Container(vs_hdr, {}, vs_words);

    // Pixel shader: export RGBA (0.25, 0.50, 0.75, 1.0) -> (64, 128, 191, 255)
    std::vector<uint32_t> ps_words;
    ps_words.push_back(enc::Vop1(0x01, 0, enc::kSrcLiteral));
    ps_words.push_back(FloatBits(0.25f));
    ps_words.push_back(enc::Vop1(0x01, 1, enc::kSrcLiteral));
    ps_words.push_back(FloatBits(0.50f));
    ps_words.push_back(enc::Vop1(0x01, 2, enc::kSrcLiteral));
    ps_words.push_back(FloatBits(0.75f));
    ps_words.push_back(enc::Vop1(0x01, 3, 242));                     // 1.0f
    EmitPair(ps_words, enc::Exp(0, 0xF, 0, 1, 2, 3, true, true));    // exp mrt0, v0, v1, v2, v3 done vm
    ps_words.push_back(enc::Sopp(0x01, 0));

    Xs2Header ps_hdr{};
    ps_hdr.stage = Xs2Stage::Pixel;
    ps_hdr.wave_mode = Xs2WaveMode::Wave32;
    ps_hdr.sgpr_count = 4;
    ps_hdr.vgpr_count = 4;
    const std::vector<uint8_t> ps_xs2 = BuildXs2Container(ps_hdr, {}, ps_words);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.PreferWarp = 1;
    uint64_t dev_handle = 0;
    SX_REQUIRE(D3D12XboxCreateSyntheticDevice(&dev_params, &dev_handle) == 0);

    D3D12XBOX_ROOT_SIGNATURE_DESC gfx_rs_desc{};
    uint64_t gfx_rs = 0;
    SX_REQUIRE(D3D12XboxCreateRootSignature(dev_handle, &gfx_rs_desc, &gfx_rs) == 0);

    D3D12XBOX_GRAPHICS_PSO_DESC gfx_desc{};
    gfx_desc.RootSignatureHandle = gfx_rs;
    gfx_desc.VS = {vs_xs2.data(), vs_xs2.size()};
    gfx_desc.PS = {ps_xs2.data(), ps_xs2.size()};
    gfx_desc.RtvFormat = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    uint64_t gfx_pso = 0;
    SX_REQUIRE(D3D12XboxCreateGraphicsPipelineState(dev_handle, &gfx_desc, &gfx_pso) == 0);
    SX_REQUIRE(gfx_pso != 0);

    const std::string ppm_path = std::string(SCARLETTX_BINARY_DIR) + "/rdna2_xs2_render.ppm";
    uint64_t pixel_hash = 0;
    SX_REQUIRE(D3D12XboxRenderWithPipelineState(dev_handle, gfx_pso, 64, 64, 3, ppm_path.c_str(), &pixel_hash) == 0);
    SX_REQUIRE(pixel_hash != 0);

    std::ifstream ifs(ppm_path, std::ios::binary);
    SX_REQUIRE(ifs.good());
    std::string magic;
    int w = 0, h = 0, maxval = 0;
    ifs >> magic >> w >> h >> maxval;
    ifs.get();  // newline
    SX_REQUIRE(magic == "P6" && w == 64 && h == 64 && maxval == 255);
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3u);
    ifs.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    SX_REQUIRE(ifs.good());

    const size_t center = (32u * 64u + 32u) * 3u;
    SX_REQUIRE(rgb[center + 0] == 64);
    SX_REQUIRE(rgb[center + 1] == 128);
    SX_REQUIRE(rgb[center + 2] == 191);

    SX_REQUIRE(D3D12XboxDestroyPipelineState(dev_handle, gfx_pso) == 0);
    SX_REQUIRE(D3D12XboxDestroyRootSignature(dev_handle, gfx_rs) == 0);
    SX_REQUIRE(D3D12XboxDestroyDevice(dev_handle) == 0);
    return true;
}

}  // namespace

int main() {
    SX_REQUIRE(RunTest1DecoderAndDisassembler());
    SX_REQUIRE(RunTest2ControlFlowAndBranching());
    SX_REQUIRE(RunTest3LiveD3D12ComputeWarp());
    SX_REQUIRE(RunTest4LiveD3D12GraphicsWarp());
    std::cout << "[PASS] test_rdna2_xs2 (GFX10.3 decoder, CFG/branching, live D3D12 WARP compute & graphics)\n";
    return 0;
}
