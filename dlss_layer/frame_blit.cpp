#include "frame_blit.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <cstdarg>
#include <cstdio>
#include <map>

#pragma comment(lib, "d3dcompiler.lib")

namespace frame_blit {
namespace {

char g_error[512] = "";

void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
}

// A straight copy. The conversion is the point: reading through a typed SRV and
// writing through a typed UAV lets the hardware do it, so the shader itself
// stays a copy and the formats carry the meaning.
const char kComputeSource[] =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "cbuffer Size : register(b0) { uint2 extent; };\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= extent.x || id.y >= extent.y) return;\n"
    "    dst[id.xy] = src[id.xy];\n"
    "}\n";

// A full-screen triangle rather than a quad: three vertices, no vertex buffer,
// and no seam down the diagonal.
const char kGraphicsSource[] =
    "Texture2D<float4> src : register(t0);\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    int2 p = int2(i.pos.xy);\n"
    "    return src.Load(int3(p, 0));\n"
    "}\n";

struct State {
    ID3D12Device *device = nullptr;
    ID3D12RootSignature *compute_root = nullptr;
    ID3D12PipelineState *compute_pso = nullptr;
    ID3D12RootSignature *graphics_root = nullptr;
    // One graphics pipeline per destination format: the render target format is
    // baked into a pipeline state and a game may present in any of several.
    std::map<int, ID3D12PipelineState *> graphics_pso;
    ID3DBlob *vs = nullptr;
    ID3DBlob *ps = nullptr;

    ID3D12DescriptorHeap *view_heap = nullptr; // shader visible: SRV then UAV
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    UINT view_stride = 0;
    bool ready = false;
};

State g;

bool compile(const char *source, size_t length, const char *entry, const char *target,
             ID3DBlob **out) {
    ID3DBlob *errors = nullptr;
    const HRESULT hr = D3DCompile(source, length, nullptr, nullptr, nullptr, entry, target, 0, 0,
                                  out, &errors);
    if (FAILED(hr)) {
        set_error("%s failed to compile: %s", entry,
                  errors ? (const char *)errors->GetBufferPointer() : "no message");
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    return true;
}

bool make_root_signature(const D3D12_ROOT_SIGNATURE_DESC &desc, ID3D12RootSignature **out) {
    ID3DBlob *blob = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) {
        set_error("root signature: %s",
                  errors ? (const char *)errors->GetBufferPointer() : "serialisation failed");
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    hr = g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                       IID_PPV_ARGS(out));
    blob->Release();
    if (FAILED(hr)) {
        set_error("CreateRootSignature failed: 0x%08lX", hr);
        return false;
    }
    return true;
}

// The graphics pipeline is built the first time a destination format is seen
// and kept, because a game presents in one format for its whole run.
ID3D12PipelineState *graphics_pipeline_for(int format) {
    auto it = g.graphics_pso.find(format);
    if (it != g.graphics_pso.end()) return it->second;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g.graphics_root;
    pso.VS = {g.vs->GetBufferPointer(), g.vs->GetBufferSize()};
    pso.PS = {g.ps->GetBufferPointer(), g.ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = (DXGI_FORMAT)format;
    pso.SampleDesc.Count = 1;

    ID3D12PipelineState *state = nullptr;
    if (FAILED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&state)))) {
        set_error("no graphics pipeline for destination format %d", format);
        return nullptr;
    }
    g.graphics_pso[format] = state;
    return state;
}

} // namespace

bool init(ID3D12Device *device) {
    if (g.ready) return true;
    g.device = device;

    ID3DBlob *cs = nullptr;
    if (!compile(kComputeSource, sizeof kComputeSource - 1, "main", "cs_5_0", &cs)) return false;
    if (!compile(kGraphicsSource, sizeof kGraphicsSource - 1, "vs_main", "vs_5_0", &g.vs) ||
        !compile(kGraphicsSource, sizeof kGraphicsSource - 1, "ps_main", "ps_5_0", &g.ps)) {
        cs->Release();
        return false;
    }

    // Compute: one descriptor table holding the source SRV and the destination
    // UAV, plus the extent as root constants.
    D3D12_DESCRIPTOR_RANGE compute_ranges[2] = {};
    compute_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_ranges[0].NumDescriptors = 1;
    compute_ranges[0].BaseShaderRegister = 0;
    compute_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_ranges[1].NumDescriptors = 1;
    compute_ranges[1].BaseShaderRegister = 0;
    compute_ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER compute_params[2] = {};
    compute_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compute_params[0].DescriptorTable.NumDescriptorRanges = 2;
    compute_params[0].DescriptorTable.pDescriptorRanges = compute_ranges;
    compute_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    compute_params[1].Constants.Num32BitValues = 2;
    compute_params[1].Constants.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC compute_desc{};
    compute_desc.NumParameters = 2;
    compute_desc.pParameters = compute_params;
    if (!make_root_signature(compute_desc, &g.compute_root)) {
        cs->Release();
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_pso{};
    compute_pso.pRootSignature = g.compute_root;
    compute_pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    const HRESULT hr = device->CreateComputePipelineState(&compute_pso, IID_PPV_ARGS(&g.compute_pso));
    cs->Release();
    if (FAILED(hr)) {
        set_error("CreateComputePipelineState failed: 0x%08lX", hr);
        return false;
    }

    // Graphics: just the source SRV.
    D3D12_DESCRIPTOR_RANGE graphics_range{};
    graphics_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    graphics_range.NumDescriptors = 1;
    graphics_range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER graphics_param{};
    graphics_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphics_param.DescriptorTable.NumDescriptorRanges = 1;
    graphics_param.DescriptorTable.pDescriptorRanges = &graphics_range;
    graphics_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC graphics_desc{};
    graphics_desc.NumParameters = 1;
    graphics_desc.pParameters = &graphics_param;
    graphics_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (!make_root_signature(graphics_desc, &g.graphics_root)) return false;

    // Two slots are enough: each pass binds its own views immediately before
    // using them, and the passes never overlap.
    D3D12_DESCRIPTOR_HEAP_DESC view_heap{};
    view_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_heap.NumDescriptors = 2;
    view_heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&view_heap, IID_PPV_ARGS(&g.view_heap)))) {
        set_error("CreateDescriptorHeap (views) failed");
        return false;
    }
    g.view_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{};
    rtv_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&g.rtv_heap)))) {
        set_error("CreateDescriptorHeap (render targets) failed");
        return false;
    }

    g.ready = true;
    return true;
}

bool to_shared(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src, ID3D12Resource *dst,
               unsigned width, unsigned height) {
    if (!g.ready) {
        set_error("frame_blit::init has not run");
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.view_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = src->GetDesc().Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g.device->CreateShaderResourceView(src, &srv, cpu);

    cpu.ptr += g.view_stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = dst->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu);

    ID3D12DescriptorHeap *heaps[] = {g.view_heap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(g.compute_root);
    cmd->SetPipelineState(g.compute_pso);
    cmd->SetComputeRootDescriptorTable(0, g.view_heap->GetGPUDescriptorHandleForHeapStart());
    const UINT extent[2] = {width, height};
    cmd->SetComputeRoot32BitConstants(1, 2, extent, 0);
    cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

bool to_backbuffer(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src, ID3D12Resource *dst,
                   unsigned width, unsigned height, int dst_format) {
    if (!g.ready) {
        set_error("frame_blit::init has not run");
        return false;
    }
    ID3D12PipelineState *pso = graphics_pipeline_for(dst_format);
    if (!pso) return false;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.view_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = src->GetDesc().Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g.device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = (DXGI_FORMAT)dst_format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.device->CreateRenderTargetView(dst, &rtv_desc, rtv);

    ID3D12DescriptorHeap *heaps[] = {g.view_heap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(g.graphics_root);
    cmd->SetPipelineState(pso);
    cmd->SetGraphicsRootDescriptorTable(0, g.view_heap->GetGPUDescriptorHandleForHeapStart());

    D3D12_VIEWPORT viewport{0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    return true;
}

void shutdown() {
    for (auto &entry : g.graphics_pso)
        if (entry.second) entry.second->Release();
    g.graphics_pso.clear();
    if (g.compute_pso) g.compute_pso->Release();
    if (g.compute_root) g.compute_root->Release();
    if (g.graphics_root) g.graphics_root->Release();
    if (g.vs) g.vs->Release();
    if (g.ps) g.ps->Release();
    if (g.view_heap) g.view_heap->Release();
    if (g.rtv_heap) g.rtv_heap->Release();
    g = State{};
}

const char *last_error() { return g_error; }

} // namespace frame_blit
