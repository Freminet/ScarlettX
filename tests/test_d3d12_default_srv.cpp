// Read each slice and mip through a default SRV on a typeless-promoted texture.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "sx_check.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

enum DeviceSlot {
    CreateQueue = 8, CreateAllocator = 9, CreateComputePso = 11,
    CreateList = 12, CreateDescriptorHeap = 14, CreateRootSignature = 16,
    CreateSrv = 18, CreateResource = 27, CreateFence = 36
};
enum ListSlot {
    Close = 9, Dispatch = 14, CopyBuffer = 15, CopyTexture = 16,
    Barrier = 26, SetHeaps = 32, SetComputeRootSignature = 33,
    SetComputeTable = 35, SetComputeUav = 45
};
enum ResourceSlot { Map = 8, Unmap = 9, GetDesc = 10, GetAddress = 11 };
enum HeapSlot { GetCpuStart = 9, GetGpuStart = 10 };
enum QueueSlot { Submit = 10, Signal = 14 };
constexpr int kCompletedValue = 8;

constexpr int kRelease = 2;
constexpr uintptr_t kZero = 0;
constexpr unsigned kMipCount = 2;
constexpr unsigned kTextureSize = 4;
constexpr unsigned kPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr unsigned kSubresourceStride = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr std::array<std::array<uint8_t, 4>, 4> kColors{{
    {{17, 31, 63, 255}}, {{29, 57, 97, 255}},
    {{43, 71, 111, 255}}, {{59, 83, 127, 255}},
}};
using Pixel = std::array<uint32_t, 4>;

using ProxySlot = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, uintptr_t, uintptr_t, uintptr_t);

uintptr_t ProxyArgument(uintptr_t value) { return value; }

template <typename T>
uintptr_t ProxyArgument(T* pointer) {
    return reinterpret_cast<uintptr_t>(pointer);
}

// Proxy methods take word-sized arguments. Extended slots use the same
// signature with additional words, stored in the vtable as ProxySlot.
template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    const auto* vtable = *static_cast<const ProxySlot* const*>(object);
    using Method = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, decltype(ProxyArgument(args))...);
    return reinterpret_cast<Method>(vtable[slot])(object, ProxyArgument(args)...);
}

void* MakeResource(void* device, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc,
                   D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    void* resource = nullptr;
    SX_REQUIRE(Call(device, CreateResource, &properties, kZero, &desc,
                    static_cast<uintptr_t>(state), kZero, kZero, &resource) == 0);
    SX_REQUIRE(resource);
    return resource;
}

D3D12_RESOURCE_DESC Buffer(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

void Transition(void* list, void* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = static_cast<ID3D12Resource*>(resource);
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    Call(list, Barrier, uintptr_t{1}, &barrier, kZero);
}

void UploadTexture(void* list, void* texture, void* upload, unsigned subresources) {
    void* data = nullptr;
    SX_REQUIRE(Call(upload, Map, kZero, kZero, &data) == 0 && data);
    auto* bytes = static_cast<uint8_t*>(data);
    // The shader reads only texel (0, 0) from each subresource.
    for (unsigned i = 0; i < subresources; ++i) {
        std::memcpy(bytes + i * kSubresourceStride, kColors[i].data(), kColors[i].size());
    }
    Call(upload, Unmap, kZero, kZero, kZero);

    for (unsigned i = 0; i < subresources; ++i) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = static_cast<ID3D12Resource*>(upload);
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = i * kSubresourceStride;
        src.PlacedFootprint.Footprint = {kFormat, 1, 1, 1, kPitch};
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = static_cast<ID3D12Resource*>(texture);
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        Call(list, CopyTexture, &dst, kZero, kZero, kZero, &src, kZero);
    }
    Transition(list, texture, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void MakePipeline(void* device, bool single, void** root, void** pipeline) {
    constexpr char arrayShader[] = R"(
        Texture2DArray<float4> source : register(t0);
        RWStructuredBuffer<uint4> results : register(u0);
        [numthreads(1, 1, 1)]
        void main() {
            for (uint i = 0; i < 4; ++i) {
                results[i] = (uint4)round(source.Load(int4(0, 0, i / 2, i % 2)) * 255);
            }
        }
    )";
    constexpr char singleShader[] = R"(
        Texture2D<float4> source : register(t0);
        RWStructuredBuffer<uint4> results : register(u0);
        [numthreads(1, 1, 1)]
        void main() {
            for (uint i = 0; i < 2; ++i) {
                results[i] = (uint4)round(source.Load(int3(0, 0, i)) * 255);
            }
        }
    )";
    const std::string_view shader = single ? singleShader : arrayShader;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT compile = D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr,
                                       "main", "cs_5_0", 0, 0, &code, &errors);
    if (errors) {
        std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }
    SX_REQUIRE(SUCCEEDED(compile) && code);

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = parameters;
    ID3DBlob* serialized = nullptr;
    SX_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                    &serialized, nullptr)));
    SX_REQUIRE(Call(device, CreateRootSignature, kZero, serialized->GetBufferPointer(),
                    serialized->GetBufferSize(), kZero, root) == 0);
    serialized->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = static_cast<ID3D12RootSignature*>(*root);
    psoDesc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    SX_REQUIRE(Call(device, CreateComputePso, &psoDesc, kZero, pipeline) == 0);
    code->Release();
    SX_REQUIRE(*root && *pipeline);
}

D3D12_SHADER_RESOURCE_VIEW_DESC NullSrv(unsigned slices) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = kFormat;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (slices == 1) {
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D = {};
        desc.Texture2D.MipLevels = kMipCount;
    } else {
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {};
        desc.Texture2DArray.MipLevels = kMipCount;
        desc.Texture2DArray.ArraySize = slices;
    }
    return desc;
}

bool Verify(void* readback, unsigned subresources) {
    void* mapped = nullptr;
    SX_REQUIRE(Call(readback, Map, kZero, kZero, &mapped) == 0 && mapped);
    bool match = true;
    for (unsigned i = 0; i < subresources; ++i) {
        Pixel got{};
        std::memcpy(got.data(), static_cast<const uint8_t*>(mapped) + i * sizeof(Pixel),
                    sizeof(Pixel));
        const auto& expected = kColors[i];
        const bool correct = got == Pixel{expected[0], expected[1], expected[2], expected[3]};
        match &= correct;
        std::printf("slice=%u mip=%u expected=%u,%u,%u,%u actual=%u,%u,%u,%u %s\n",
                    i / kMipCount, i % kMipCount, unsigned(expected[0]), unsigned(expected[1]),
                    unsigned(expected[2]), unsigned(expected[3]), got[0], got[1], got[2], got[3],
                    correct ? "PASS" : "FAIL");
    }
    Call(readback, Unmap, kZero, kZero, kZero);
    return match;
}

}  // namespace

int main(int argc, char** argv) {
    const bool single = argc == 2 && std::string_view(argv[1]) == "single";
    SX_REQUIRE(argc == 1 || single);
    const unsigned slices = single ? 1 : 2;
    const unsigned subresources = slices * kMipCount;
    const UINT64 resultBytes = subresources * sizeof(Pixel);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    const IID iid = __uuidof(ID3D12Device);
    void* device = nullptr;
    SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &iid, &device) >= 0 && device);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    void* queue = nullptr;
    void* allocator = nullptr;
    void* list = nullptr;
    void* fence = nullptr;
    SX_REQUIRE(Call(device, CreateQueue, &queueDesc, kZero, &queue) == 0 && queue);
    SX_REQUIRE(Call(device, CreateAllocator, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                    kZero, &allocator) == 0 && allocator);
    SX_REQUIRE(Call(device, CreateFence, kZero, kZero, kZero, &fence) == 0 && fence);
    void* root = nullptr;
    void* pipeline = nullptr;
    MakePipeline(device, single, &root, &pipeline);
    SX_REQUIRE(Call(device, CreateList, kZero, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                    allocator, pipeline, kZero, &list) == 0 && list);

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = kTextureSize;
    textureDesc.Height = kTextureSize;
    textureDesc.DepthOrArraySize = static_cast<UINT16>(slices);
    textureDesc.MipLevels = kMipCount;
    textureDesc.Format = kFormat;
    textureDesc.SampleDesc.Count = 1;
    void* texture = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT, textureDesc,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
    // Verify that the test exercises descriptor synthesis on a typeless resource.
    D3D12_RESOURCE_DESC hostDesc{};
    Call(texture, GetDesc, &hostDesc, kZero, kZero);
    SX_REQUIRE(hostDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS);

    void* upload = MakeResource(device, D3D12_HEAP_TYPE_UPLOAD,
                                Buffer(subresources * kSubresourceStride, D3D12_RESOURCE_FLAG_NONE),
                                D3D12_RESOURCE_STATE_GENERIC_READ);
    void* output = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT,
                                Buffer(resultBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    void* readback = MakeResource(device, D3D12_HEAP_TYPE_READBACK,
                                  Buffer(resultBytes, D3D12_RESOURCE_FLAG_NONE),
                                  D3D12_RESOURCE_STATE_COPY_DEST);
    UploadTexture(list, texture, upload, subresources);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    void* heap = nullptr;
    SX_REQUIRE(Call(device, CreateDescriptorHeap, &heapDesc, kZero, &heap) == 0 && heap);
    const auto cpu = static_cast<uintptr_t>(Call(heap, GetCpuStart, kZero, kZero, kZero));
    const auto gpu = static_cast<uintptr_t>(Call(heap, GetGpuStart, kZero, kZero, kZero));
    SX_REQUIRE(cpu && gpu);
    // Give the shader a defined null binding if descriptor creation fails.
    const auto nullDesc = NullSrv(slices);
    Call(device, CreateSrv, kZero, &nullDesc, cpu);
    Call(device, CreateSrv, texture, kZero, cpu);

    Call(list, SetComputeRootSignature, root, kZero, kZero);
    Call(list, SetHeaps, uintptr_t{1}, &heap, kZero);
    Call(list, SetComputeTable, kZero, gpu, kZero);
    const auto outputAddress = static_cast<uintptr_t>(Call(output, GetAddress, kZero, kZero, kZero));
    SX_REQUIRE(outputAddress);
    Call(list, SetComputeUav, uintptr_t{1}, outputAddress, kZero);
    Call(list, Dispatch, uintptr_t{1}, uintptr_t{1}, uintptr_t{1});
    Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Call(list, CopyBuffer, readback, kZero, output, kZero, static_cast<uintptr_t>(resultBytes));
    SX_REQUIRE(Call(list, Close, kZero, kZero, kZero) == 0);
    Call(queue, Submit, uintptr_t{1}, &list, kZero);
    SX_REQUIRE(Call(queue, Signal, fence, uintptr_t{1}, kZero) == 0);
    const ULONGLONG start = GetTickCount64();
    intptr_t completed = 0;
    while ((completed = Call(fence, kCompletedValue, kZero, kZero, kZero)) == 0) {
        SX_REQUIRE(GetTickCount64() - start < 5000);
        Sleep(1);
    }
    // A removed D3D12 device reports UINT64_MAX instead of the signaled value.
    SX_REQUIRE(completed == 1);

    const bool matches = Verify(readback, subresources);
    const std::array objects{list, fence, heap, readback, output, upload, texture,
                             pipeline, root, allocator, queue, device};
    for (void* object : objects) {
        Call(object, kRelease, kZero, kZero, kZero);
    }
    SX_REQUIRE(matches);
    return 0;
}
