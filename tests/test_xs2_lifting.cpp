#include "scarlettx/gpu/xs2_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
    std::cout << "[test_xs2_lifting] Testing RDNA2 XS2 Shader Lifting...\n";

    const uint8_t sample_microcode[] = {
        0x00, 0x00, 0x80, 0xC0, // s_load_dwordx4
        0x01, 0x02, 0x00, 0x7E, // v_add_f32
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0xBF  // s_endpgm
    };
    size_t microcode_size = sizeof(sample_microcode);

    // 1. Identification
    XS2_MICROCODE_INFO info{};
    SX_CHECK(Xs2ShaderIdentifyMicrocode(sample_microcode, microcode_size, &info) == 0);
    SX_CHECK(info.instructionCount >= 4);
    SX_CHECK(info.shaderHash != 0);

    // 2. Disassembly
    char disasm[256]{};
    size_t actual_disasm = 0;
    SX_CHECK(Xs2ShaderDisassembleRdna2(sample_microcode, microcode_size, disasm, sizeof(disasm), &actual_disasm) == 0);
    SX_CHECK(actual_disasm > 0);
    SX_CHECK(std::strstr(disasm, "s_endpgm") != nullptr);

    // 3. Lift to DXIL & SPIR-V
    XS2_TRANSLATION_OPTIONS opts{};
    opts.targetShaderModel = 0x00060005;
    opts.enableWave32 = 1;

    void* dxil_buf = nullptr;
    size_t dxil_size = 0;
    SX_CHECK(Xs2ShaderLiftToDxil(sample_microcode, microcode_size, &opts, &dxil_buf, &dxil_size) == 0);
    SX_CHECK(dxil_buf != nullptr);
    SX_CHECK(dxil_size > 0);

    int32_t is_valid_dxil = 0;
    SX_CHECK(Xs2ShaderValidateDxilContainer(dxil_buf, dxil_size, &is_valid_dxil) == 0);
    SX_CHECK(is_valid_dxil == 1);
    SX_CHECK(Xs2ShaderFreeBytecode(dxil_buf) == 0);

    void* spirv_buf = nullptr;
    size_t spirv_size = 0;
    SX_CHECK(Xs2ShaderLiftToSpirv(sample_microcode, microcode_size, &opts, &spirv_buf, &spirv_size) == 0);
    SX_CHECK(spirv_buf != nullptr);
    SX_CHECK(spirv_size > 0);
    SX_CHECK(Xs2ShaderFreeBytecode(spirv_buf) == 0);

    // 4. Resource bindings & chunk stripping
    XS2_RESOURCE_BINDINGS bindings{};
    SX_CHECK(Xs2ShaderExtractResourceBindings(sample_microcode, microcode_size, &bindings) == 0);
    SX_CHECK(bindings.srvCount == 4);

    void* stripped = nullptr;
    size_t stripped_size = 0;
    SX_CHECK(Xs2ShaderStripXboxVendorChunks(sample_microcode, microcode_size, &stripped, &stripped_size) == 0);
    SX_CHECK(stripped != nullptr);
    delete[] static_cast<uint8_t*>(stripped);

    // 5. Wavefront & workgroup queries
    XS2_WAVEFRONT_PROPS props{};
    SX_CHECK(Xs2ShaderQueryWavefrontProperties(sample_microcode, microcode_size, &props) == 0);
    SX_CHECK(props.waveSize == 32);

    uint32_t wx = 0, wy = 0, wz = 0;
    SX_CHECK(Xs2ShaderTranslateComputeWorkgroupSize(sample_microcode, microcode_size, &wx, &wy, &wz) == 0);
    SX_CHECK(wx == 8 && wy == 8);

    // 6. Translation pipeline & caching
    XS2_PIPELINE_DESC pipe_desc{};
    pipe_desc.maxCacheEntries = 100;
    XS2_PIPELINE_HANDLE pipeline = nullptr;
    SX_CHECK(Xs2ShaderCreateTranslationPipeline(&pipe_desc, &pipeline) == 0);
    SX_CHECK(pipeline != nullptr);

    void* cached_bc1 = nullptr;
    size_t cached_sz1 = 0;
    SX_CHECK(Xs2ShaderTranslateCached(pipeline, sample_microcode, microcode_size, &cached_bc1, &cached_sz1) == 0);
    delete[] static_cast<uint8_t*>(cached_bc1);

    // Second call hits cache
    void* cached_bc2 = nullptr;
    size_t cached_sz2 = 0;
    SX_CHECK(Xs2ShaderTranslateCached(pipeline, sample_microcode, microcode_size, &cached_bc2, &cached_sz2) == 0);
    delete[] static_cast<uint8_t*>(cached_bc2);

    XS2_TRANSLATION_STATS stats{};
    SX_CHECK(Xs2ShaderGetTranslationStatistics(pipeline, &stats) == 0);
    SX_CHECK(stats.totalShadersTranslated == 1);
    SX_CHECK(stats.cacheHits == 1);

    SX_CHECK(Xs2ShaderClearPipelineCache(pipeline) == 0);
    SX_CHECK(Xs2ShaderDestroyTranslationPipeline(pipeline) == 0);

    std::cout << "[test_xs2_lifting] All 16 Xs2Shader APIs verified successfully!\n";
    return 0;
}
