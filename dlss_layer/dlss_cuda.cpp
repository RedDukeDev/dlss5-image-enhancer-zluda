#include "dlss_cuda.h"
#include "cuda_min.h"
#include "ngx_cuda.h"
#include "frame_blit.h"

#include <d3d12.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cstring>
#include <string>
#include <vector>

namespace {

char g_error[512] = "";

// Set by the ReShade addon so NGX's own messages reach ReShade's log, which is
// the only place a user can see them.
void (*g_reshade_log)(const char *) = nullptr;

void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
}

// What a resource parameter has to point at.
//
// [RE] The snippet does not take a bare CUDA handle. nvngx_dlss.dll+0xB3550
// dereferences the parameter (`mov rdx, [rdi]`) and passes the first eight
// bytes to cuTexObjectGetResourceDesc, then to cuSurfObjectGetResourceDesc if
// that fails; when both fail the evaluation ends with 0xBAD00002. Handing it a
// mipmapped array meant it read the first field of the driver's own object and
// queried that, which is meaningless.
//
// So the parameter is a pointer to a descriptor whose first field is a texture
// or surface object. Only that field is read before the query, and the query
// itself then requires resType == CU_RESOURCE_TYPE_ARRAY and a working
// cuArrayGetDescriptor -- which a surface built on a mip level satisfies.
struct NgxResourceCuda {
    unsigned long long object; // CUsurfObject or CUtexObject
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
};

// One texture this layer owns, shared out to CUDA. The game's own resources are
// not created with D3D12_HEAP_FLAG_SHARED and cannot be imported, so each frame
// is copied into one of these first.
struct SharedTexture {
    ID3D12Resource *resource = nullptr;
    HANDLE nt_handle = nullptr;
    CUexternalMemory extmem = nullptr;
    CUmipmappedArray mipmap = nullptr;
    CUarray level0 = nullptr;       // mip level 0 of the above
    CUsurfObject surface = 0;       // a surface built on level0
    CUtexObject texture = 0;        // a texture built on the same array
    // Whether the snippet may write to this image. [RE] 0xB3550 answers with a
    // flag word: 1 for a texture object, 5 for a surface object, and the caller
    // tests bit 0 for "known" and bit 2 for "writable" -- so a texture object is
    // read only and only a surface object can be a destination. Getting this
    // wrong on the output is FAIL_RWFlagMissing, 0xBAD00009.
    bool read_write = false;
    NgxResourceCuda descriptor{};   // what the parameter points at
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct State {
    HMODULE nvcuda = nullptr;
    HMODULE nvapi = nullptr;
    HMODULE ngx = nullptr;
    CudaApi cu{};

    CUcontext ctx = nullptr;

    // Entry points of our own NGX runtime (nvngx.dll), not of the snippet: the
    // snippet refuses any caller whose module is not named nvngx.dll, so every
    // NVSDK_NGX_CUDA_* call has to originate inside that DLL.
    const char *(*ngx_load)(const wchar_t *) = nullptr;
    // The runtime builds the NVSDK_NGX_Parameter the snippet reads its logging
    // settings from, so only the sink and its level cross this boundary.
    NVSDK_NGX_Result (*ngx_init_ext)(unsigned long long, const wchar_t *, NVSDK_NGX_Version,
                                     unsigned long long, unsigned, void *) = nullptr;
    NVSDK_NGX_Result (*ngx_populate)(NVSDK_NGX_Parameter **) = nullptr;
    NVSDK_NGX_Result (*ngx_create)(NVSDK_NGX_Feature, NVSDK_NGX_Parameter *,
                                   NVSDK_NGX_Handle **) = nullptr;
    NVSDK_NGX_Result (*ngx_evaluate)(NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *) = nullptr;
    NVSDK_NGX_Result (*ngx_release)(NVSDK_NGX_Handle *) = nullptr;
    NVSDK_NGX_Result (*ngx_shutdown)(void) = nullptr;

    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;

    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;

    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *cmd = nullptr;

    std::wstring snippet_path;
    SharedTexture color, depth, motion, output;
    dlss_cuda::FeatureDesc current{};
    bool initialized = false;
};

State g;

bool cu_ok(CUresult r, const char *what) {
    if (r == CUDA_SUCCESS) return true;
    const char *msg = nullptr;
    if (g.cu.cuGetErrorString) g.cu.cuGetErrorString(r, &msg);
    set_error("%s failed: %d (%s)", what, r, msg ? msg : "?");
    return false;
}

// The snippet's own log. It reports fatal conditions here just before calling
// exit(1), so losing these messages means losing the only explanation.
void __cdecl ngx_log_sink(const char *message, int level, unsigned feature) {
    char line[1024];
    snprintf(line, sizeof line, "[ngx %d/%u] %s\n", level, feature, message ? message : "(null)");
    OutputDebugStringA(line);
    // Also to stderr: in a game that goes nowhere, but it is what makes the
    // messages visible to a console harness, and they are the only explanation
    // available before the snippet terminates the process.
    fputs(line, stderr);
    fflush(stderr);
    if (g_reshade_log) g_reshade_log(line);
}

// Which file a load actually produced. LoadLibrary matches an already-loaded
// module by base name, so asking for our nvngx.dll in a game that ships its own
// hands back the game's -- with the same exported names, so the mistake only
// surfaces much later as an unexplained refusal.
void report_module(const wchar_t *requested, HMODULE loaded) {
    wchar_t actual[MAX_PATH] = L"?";
    if (loaded) GetModuleFileNameW(loaded, actual, MAX_PATH);
    char line[1024];
    snprintf(line, sizeof line, "[dlss-cuda] loaded %ls\n", actual);
    OutputDebugStringA(line);
    fputs(line, stderr);
    if (g_reshade_log) g_reshade_log(line);
    // Compare canonical paths: the request may use forward slashes or be a bare
    // name, and neither difference means a different file.
    wchar_t wanted[MAX_PATH] = L"";
    if (requested) GetFullPathNameW(requested, MAX_PATH, wanted, nullptr);
    if (requested && _wcsicmp(wanted, actual) != 0) {
        snprintf(line, sizeof line,
                 "[dlss-cuda] WARNING: asked for %ls but the process already had a module of "
                 "that name; that one is being used instead\n", requested);
        OutputDebugStringA(line);
        fputs(line, stderr);
        if (g_reshade_log) g_reshade_log(line);
    }
}

// How many devices the CUDA side of the snippet has registered.
//
// [RE] Each backend's CreateFeatureCommon keeps its own {_Myhead, _Mysize} map;
// the CUDA one is at nvngx_dlssnr.dll +0x1152BF8, so the count sits at
// +0x1152C00. Verified on the bench: one with only our device, two with a
// foreign one alongside. The offset belongs to v310.8.0 and nothing else, so an
// implausible value is reported as unknown rather than trusted.
void report_device_count(const char *when) {
    const HMODULE snippet = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (!snippet) return;
    const unsigned long long count =
        *reinterpret_cast<const unsigned long long *>((const unsigned char *)snippet + 0x1152C00);
    char line[192];
    if (count > 16)
        snprintf(line, sizeof line, "[dlss-cuda] registered CUDA devices (%s): unreadable\n", when);
    else
        snprintf(line, sizeof line, "[dlss-cuda] registered CUDA devices (%s): %llu\n", when, count);
    OutputDebugStringA(line);
    fputs(line, stderr);
    if (g_reshade_log) g_reshade_log(line);
}

void report_prior_snippet(const wchar_t *name) {
    if (!GetModuleHandleW(name)) return;
    char line[512];
    snprintf(line, sizeof line,
             "[dlss-cuda] WARNING: %ls was already loaded before us; another user of this "
             "snippet in the process makes CreateFeature refuse to pick a device\n", name);
    OutputDebugStringA(line);
    fputs(line, stderr);
    if (g_reshade_log) g_reshade_log(line);
}

// Whether the snippet's PTX is readable at all.
//
// A stand-in driver compiles PTX and can do nothing with a Zstandard block, so
// against a copy that carries no uncompressed PTX every module translates to
// nothing: cuModuleLoadData still succeeds and the kernel lookup then fails
// with CUDA_ERROR_NOT_FOUND, which says nothing about the real cause.
//
// Measured on two copies identical in size and different in content: one holds
// fifteen uncompressed PTX blobs and the port runs end to end against it, while
// the build patched to run on RTX 20/30/40 parts holds none and fails exactly
// this way. Uncompressed PTX is recognisable by the compiler's own command
// line, which it keeps verbatim.
//
// Only read on failure: it means scanning a very large file, and it earns that
// only when there is a failure to explain.
void report_snippet_compression(const wchar_t *path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::vector<char> buffer(1 << 20);
    std::string tail;
    bool plain_ptx = false;
    DWORD read = 0;
    while (!plain_ptx && ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, nullptr) && read) {
        std::string chunk = tail + std::string(buffer.data(), read);
        if (chunk.find("--set-te") != std::string::npos) plain_ptx = true;
        // Keep enough of the tail that a marker split across two reads is
        // still found.
        tail = chunk.size() > 16 ? chunk.substr(chunk.size() - 16) : chunk;
    }
    CloseHandle(file);
    if (plain_ptx) return;
    char line[768];
    snprintf(line, sizeof line,
             "[dlss-cuda] this copy of the snippet carries no uncompressed PTX, so nothing can "
             "be translated and every kernel lookup fails. Use a copy that does; the build "
             "patched for RTX 20/30/40 parts does not, and this port does not need it -- the "
             "architecture check is already answered by our NVAPI stand-in.\n");
    OutputDebugStringA(line);
    fputs(line, stderr);
    if (g_reshade_log) g_reshade_log(line);
}

// The runtime is missing one of its exports.
//
// Worth a sentence rather than a code: these exports belong to this project's
// own nvngx.dll, built from ngx_runtime/ngx_runtime.cpp. NVIDIA's nvngx.dll does
// not have them, nor do the nvngx_dlss*.dll snippets, and a message that named a
// fixed file sent readers looking at the wrong one.
bool missing_runtime_export(const wchar_t *path, const char *symbol) {
    set_error("%ls does not export %s. That export belongs to this project's own nvngx.dll, "
              "built from ngx_runtime/ngx_runtime.cpp -- NVIDIA's nvngx.dll and the "
              "nvngx_dlss*.dll snippets do not have it.",
              path ? path : L"nvngx.dll", symbol);
    return false;
}

bool ngx_ok(NVSDK_NGX_Result r, const char *what) {
    if (NVSDK_NGX_SUCCEED(r)) return true;
    set_error("%s failed: 0x%08X", what, r);
    return false;
}

// DXGI formats this layer shares with CUDA. Only the ones DLSS actually needs
// for colour, depth and motion vectors; anything else is rejected loudly rather
// than silently mapped to something plausible.
bool cuda_format_of(DXGI_FORMAT fmt, CUarray_format &out_format, unsigned &out_channels) {
    switch (fmt) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: out_format = CU_AD_FORMAT_HALF;  out_channels = 4; return true;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: out_format = CU_AD_FORMAT_FLOAT; out_channels = 4; return true;
    case DXGI_FORMAT_R16G16_FLOAT:       out_format = CU_AD_FORMAT_HALF;  out_channels = 2; return true;
    case DXGI_FORMAT_R32G32_FLOAT:       out_format = CU_AD_FORMAT_FLOAT; out_channels = 2; return true;
    case DXGI_FORMAT_R32_FLOAT:          out_format = CU_AD_FORMAT_FLOAT; out_channels = 1; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:     out_format = CU_AD_FORMAT_UNSIGNED_INT8; out_channels = 4; return true;
    default: return false;
    }
}

void release_shared(SharedTexture &t) {
    if (t.texture && g.cu.cuTexObjectDestroy) g.cu.cuTexObjectDestroy(t.texture);
    if (t.surface && g.cu.cuSurfObjectDestroy) g.cu.cuSurfObjectDestroy(t.surface);
    if (t.mipmap && g.cu.cuMipmappedArrayDestroy) g.cu.cuMipmappedArrayDestroy(t.mipmap);
    if (t.extmem && g.cu.cuDestroyExternalMemory) g.cu.cuDestroyExternalMemory(t.extmem);
    if (t.nt_handle) CloseHandle(t.nt_handle);
    if (t.resource) t.resource->Release();
    t = SharedTexture{};
}

// Creates a shared committed texture and imports it into CUDA as a mipmapped
// array with a single level.
bool make_shared(SharedTexture &t, uint32_t width, uint32_t height, DXGI_FORMAT format,
                 bool allow_unordered_access) {
    release_shared(t);

    CUarray_format cu_format;
    unsigned channels;
    if (!cuda_format_of(format, cu_format, channels)) {
        set_error("unsupported DXGI format %d for CUDA sharing", (int)format);
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = allow_unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                        : D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = g.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&t.resource));
    if (FAILED(hr)) {
        set_error("CreateCommittedResource(SHARED) failed: 0x%08lX", hr);
        return false;
    }

    hr = g.device->CreateSharedHandle(t.resource, nullptr, GENERIC_ALL, nullptr, &t.nt_handle);
    if (FAILED(hr)) {
        set_error("CreateSharedHandle failed: 0x%08lX", hr);
        return false;
    }

    const D3D12_RESOURCE_ALLOCATION_INFO info = g.device->GetResourceAllocationInfo(0, 1, &desc);

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC mem{};
    mem.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
    mem.handle.win32.handle = t.nt_handle;
    mem.size = info.SizeInBytes;
    // A D3D12 resource import must be flagged dedicated; the allocation backs
    // exactly this one resource.
    mem.flags = 1; // CUDA_EXTERNAL_MEMORY_DEDICATED
    if (!cu_ok(g.cu.cuImportExternalMemory(&t.extmem, &mem), "cuImportExternalMemory")) return false;

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC arr{};
    arr.offset = 0;
    arr.arrayDesc.Width = width;
    arr.arrayDesc.Height = height;
    arr.arrayDesc.Depth = 0;
    arr.arrayDesc.Format = cu_format;
    arr.arrayDesc.NumChannels = channels;
    arr.arrayDesc.Flags = 0x2; // CUDA_ARRAY3D_SURFACE_LDST
    arr.numLevels = 1;
    if (!cu_ok(g.cu.cuExternalMemoryGetMappedMipmappedArray(&t.mipmap, t.extmem, &arr),
               "cuExternalMemoryGetMappedMipmappedArray"))
        return false;

    // Level zero and a surface over it: which of the three the snippet wants is
    // selected at evaluation time.
    if (!cu_ok(g.cu.cuMipmappedArrayGetLevel(&t.level0, t.mipmap, 0), "cuMipmappedArrayGetLevel"))
        return false;
    // Can the imported array be interrogated at all? DLSS resolves
    // cuArrayGetDescriptor, so if this fails the snippet has no way to learn the
    // format of what it was handed.
    {
        CUDA_ARRAY_DESCRIPTOR ad{};
        CUresult dr = g.cu.cuArrayGetDescriptor(&ad, t.level0);
        char line[256];
        snprintf(line, sizeof line,
                 "  [array] cuArrayGetDescriptor -> %d   %zux%zu fmt=0x%X canali=%u\n",
                 dr, ad.Width, ad.Height, (unsigned)ad.Format, ad.NumChannels);
        OutputDebugStringA(line);
        fputs(line, stderr);
        fflush(stderr);
    }

    CUDA_RESOURCE_DESC rd{};
    rd.resType = CU_RESOURCE_TYPE_ARRAY;
    rd.res.array.hArray = t.level0;
    if (!cu_ok(g.cu.cuSurfObjectCreate(&t.surface, &rd), "cuSurfObjectCreate")) return false;

    // A texture object over the same array. The snippet tries
    // cuTexObjectGetResourceDesc before cuSurfObjectGetResourceDesc, so this is
    // the handle that answers on the first attempt; the surface stays for the
    // paths that write.
    if (g.cu.cuTexObjectCreate) {
        CUDA_TEXTURE_DESC td{};
        td.filterMode = 0;                  // point
        td.flags = 2;                       // read as integer, no normalisation
        for (int i = 0; i < 3; ++i) td.addressMode[i] = 1; // clamp
        if (!cu_ok(g.cu.cuTexObjectCreate(&t.texture, &rd, &td, nullptr), "cuTexObjectCreate"))
            return false;
    }

    t.width = width;
    t.height = height;
    t.format = format;
    t.read_write = allow_unordered_access;

    t.descriptor.object = t.surface;
    t.descriptor.width = width;
    t.descriptor.height = height;
    t.descriptor.pitch = 0;
    t.descriptor.format = (uint32_t)format;
    return true;
}

// What the snippet is handed for each image. Selected by environment so the
// candidates can be swept without a rebuild; the default is the one the
// disassembly of 0xB3550 calls for.
void *resource_handle(SharedTexture &t) {
    char buf[32];
    static int kind = -1;
    if (kind < 0) {
        kind = 0;
        if (GetEnvironmentVariableA("DLSS_RES_KIND", buf, sizeof buf) > 0) kind = atoi(buf);
    }
    void *handle;
    switch (kind) {
    // The three raw handles. All of them fail the same way, which is what
    // showed that the snippet was not being given a handle at all; they are
    // kept because a different snippet build may want one.
    case 1: handle = (void *)t.level0; break;
    case 2: handle = (void *)(size_t)t.surface; break;
    case 3: handle = (void *)t.mipmap; break;
    // The descriptor. Which object it names decides which of the two queries
    // answers: the texture one is tried first, the surface one is the fallback.
    case 4:
        t.descriptor.object = t.surface;
        handle = (void *)&t.descriptor;
        break;
    default:
        // Read-only images go in as texture objects, the destination as a
        // surface object, which is the distinction the snippet's flag word
        // encodes.
        t.descriptor.object =
            (!t.read_write && t.texture) ? t.texture : t.surface;
        handle = (void *)&t.descriptor;
        break;
    }
    // The snippet queries CUDA about whatever it is given, and when that query
    // fails it reports a generic error. Knowing which of our handles it was
    // asking about is the difference between a diagnosis and a guess, so the
    // three candidates are printed together with the one actually handed over.
    if (GetEnvironmentVariableA("DLSS_LOG_RES", buf, sizeof buf) > 0) {
        char line[192];
        snprintf(line, sizeof line,
                 "  [res] passato=%p   mipmap=%p  array=%p  surface=0x%llX\n", handle,
                 (void *)t.mipmap, (void *)t.level0, (unsigned long long)t.surface);
        fputs(line, stderr);
        fflush(stderr);
    }
    return handle;
}

// Blocks until the GPU has drained the queue.
bool flush_and_wait() {
    const UINT64 target = ++g.fence_value;
    if (FAILED(g.queue->Signal(g.fence, target))) {
        set_error("ID3D12CommandQueue::Signal failed");
        return false;
    }
    if (g.fence->GetCompletedValue() < target) {
        if (FAILED(g.fence->SetEventOnCompletion(target, g.fence_event))) {
            set_error("SetEventOnCompletion failed");
            return false;
        }
        WaitForSingleObject(g.fence_event, INFINITE);
    }
    return true;
}

void transition(ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g.cmd->ResourceBarrier(1, &b);
}

} // namespace

const char *cuda_api_load(void *module, CudaApi &api) {
    HMODULE h = static_cast<HMODULE>(module);
    // Try the _v2 name first: over ZLUDA several bare names are v1-ABI stubs
    // that return CUDA_ERROR_NOT_SUPPORTED.
#define LOAD(field, name)                                                    \
    do {                                                                     \
        void *p = (void *)GetProcAddress(h, name "_v2");                      \
        if (!p) p = (void *)GetProcAddress(h, name);                          \
        if (!p) return name;                                                  \
        *reinterpret_cast<void **>(&api.field) = p;                           \
    } while (0)

    LOAD(cuInit, "cuInit");
    LOAD(cuDeviceGet, "cuDeviceGet");
    LOAD(cuDeviceGetCount, "cuDeviceGetCount");
    LOAD(cuDeviceGetName, "cuDeviceGetName");
    LOAD(cuCtxCreate, "cuCtxCreate");
    LOAD(cuCtxDestroy, "cuCtxDestroy");
    LOAD(cuCtxSetCurrent, "cuCtxSetCurrent");
    // The bare name only. CUDA has no cuCtxSynchronize_v2, so what the _v2
    // lookup finds over ZLUDA is an internal variant that resolves a context of
    // its own and answers INVALID_VALUE once NGX has run, while the device is
    // fine and a copy issued right afterwards succeeds.
    *reinterpret_cast<void **>(&api.cuCtxSynchronize) =
        (void *)GetProcAddress(h, "cuCtxSynchronize");
    if (!api.cuCtxSynchronize) return "cuCtxSynchronize";
    LOAD(cuImportExternalMemory, "cuImportExternalMemory");
    LOAD(cuDestroyExternalMemory, "cuDestroyExternalMemory");
    LOAD(cuExternalMemoryGetMappedMipmappedArray, "cuExternalMemoryGetMappedMipmappedArray");
    LOAD(cuMipmappedArrayGetLevel, "cuMipmappedArrayGetLevel");
    LOAD(cuMipmappedArrayDestroy, "cuMipmappedArrayDestroy");
    LOAD(cuSurfObjectCreate, "cuSurfObjectCreate");
    LOAD(cuSurfObjectDestroy, "cuSurfObjectDestroy");
    // Optional: only the descriptor path uses them, and that path falls back to
    // the surface object when they are missing.
    *reinterpret_cast<void **>(&api.cuTexObjectCreate) =
        (void *)GetProcAddress(h, "cuTexObjectCreate");
    *reinterpret_cast<void **>(&api.cuTexObjectDestroy) =
        (void *)GetProcAddress(h, "cuTexObjectDestroy");
    LOAD(cuArrayGetDescriptor, "cuArrayGetDescriptor");
    LOAD(cuGetErrorString, "cuGetErrorString");
    *reinterpret_cast<void **>(&api.cuMemGetInfo) = (void *)GetProcAddress(h, "cuMemGetInfo_v2");
    if (!api.cuMemGetInfo)
        *reinterpret_cast<void **>(&api.cuMemGetInfo) = (void *)GetProcAddress(h, "cuMemGetInfo");
    *reinterpret_cast<void **>(&api.cuMemAlloc) = (void *)GetProcAddress(h, "cuMemAlloc_v2");
    if (!api.cuMemAlloc)
        *reinterpret_cast<void **>(&api.cuMemAlloc) = (void *)GetProcAddress(h, "cuMemAlloc");
    *reinterpret_cast<void **>(&api.cuMemcpy2D) = (void *)GetProcAddress(h, "cuMemcpy2D_v2");
    if (!api.cuMemcpy2D)
        *reinterpret_cast<void **>(&api.cuMemcpy2D) = (void *)GetProcAddress(h, "cuMemcpy2D");
#undef LOAD
    return nullptr;
}

namespace dlss_cuda {

void set_log_sink(void (*sink)(const char *)) { g_reshade_log = sink; }

bool hold_device_memory(size_t bytes) {
    if (!g.cu.cuMemAlloc) return false;
    CUdeviceptr p = 0;
    return g.cu.cuMemAlloc(&p, bytes) == CUDA_SUCCESS;
}

void *cuda_context() { return g.ctx; }

const char *last_error() { return g_error[0] ? g_error : "no error"; }

bool init(const InitDesc &desc) {
    if (g.initialized) return true;
    if (!desc.device || !desc.queue) {
        set_error("init: device and queue are required");
        return false;
    }
    g.device = desc.device;
    g.queue = desc.queue;

    // Before anything else: get our NVAPI into the process. The snippet asks
    // NVAPI how many GPUs exist and what architecture they are; with no answer it
    // reports NV_GPU_ARCHITECTURE_GK100 (Kepler), which has no tensor cores.
    // An empty path means "leave the system's NVAPI alone", which is what a real
    // NVIDIA machine wants: the stand-in exists to claim a Blackwell part on a
    // Radeon, and on an NVIDIA card it would only lie about hardware the driver
    // can describe truthfully.
    if (desc.nvapi_dll_path && desc.nvapi_dll_path[0]) {
        if (HMODULE nvapi = LoadLibraryW(desc.nvapi_dll_path)) {
            g.nvapi = nvapi;
            report_module(desc.nvapi_dll_path, nvapi);
        } else {
            set_error("could not pre-load nvapi64.dll (error %lu)", GetLastError());
            return false;
        }
    }

    g.nvcuda = LoadLibraryW(desc.nvcuda_dll_path ? desc.nvcuda_dll_path : L"nvcuda.dll");
    if (!g.nvcuda) {
        set_error("could not load the CUDA driver (error %lu)", GetLastError());
        return false;
    }
    report_module(desc.nvcuda_dll_path, g.nvcuda);
    if (const char *missing = cuda_api_load(g.nvcuda, g.cu)) {
        set_error("the CUDA driver does not export %s", missing);
        return false;
    }

    if (!cu_ok(g.cu.cuInit(0), "cuInit")) return false;
    CUdevice dev = 0;
    if (!cu_ok(g.cu.cuDeviceGet(&dev, 0), "cuDeviceGet")) return false;
    char name[256] = {};
    if (g.cu.cuDeviceGetName(name, sizeof name, dev) == CUDA_SUCCESS)
        OutputDebugStringA((std::string("[dlss-cuda] device: ") + name + "\n").c_str());
    if (!cu_ok(g.cu.cuCtxCreate(&g.ctx, CU_CTX_SCHED_AUTO, dev), "cuCtxCreate")) return false;
    if (!cu_ok(g.cu.cuCtxSetCurrent(g.ctx), "cuCtxSetCurrent")) return false;

    g.ngx = LoadLibraryW(desc.ngx_runtime_path ? desc.ngx_runtime_path : L"nvngx.dll");
    if (!g.ngx) {
        set_error("could not load the NGX runtime nvngx.dll (error %lu)", GetLastError());
        return false;
    }
    report_module(desc.ngx_runtime_path, g.ngx);

#define NGX(field, name)                                                            \
    do {                                                                            \
        *reinterpret_cast<void **>(&g.field) = (void *)GetProcAddress(g.ngx, name); \
        if (!g.field) return missing_runtime_export(desc.ngx_runtime_path, name);   \
    } while (0)
    NGX(ngx_load, "ngxrt_load");
    NGX(ngx_init_ext, "ngxrt_init");
    NGX(ngx_create, "ngxrt_create_feature");
    NGX(ngx_evaluate, "ngxrt_evaluate");
    NGX(ngx_release, "ngxrt_release_feature");
    NGX(ngx_shutdown, "ngxrt_shutdown");
    NGX(ngx_populate, "ngxrt_populate_parameters");
#undef NGX

    // Optional CreateFeature1 path, selected by environment so the two values
    // can be swept without rebuilding.
    if (auto set_inputs = (void (*)(int, unsigned long long, unsigned long long))
                              GetProcAddress(g.ngx, "ngxrt_set_inputs")) {
        char buf[64];
        const bool use = GetEnvironmentVariableA("DLSS_USE_INPUTS", buf, sizeof buf) > 0;
        unsigned long long in1 = 0, in2 = 0;
        if (GetEnvironmentVariableA("DLSS_INPUT1", buf, sizeof buf) > 0)
            in1 = (buf[0] == 'c') ? (unsigned long long)g.ctx : _strtoui64(buf, nullptr, 0);
        if (GetEnvironmentVariableA("DLSS_INPUT2", buf, sizeof buf) > 0)
            in2 = (buf[0] == 'c') ? (unsigned long long)g.ctx : _strtoui64(buf, nullptr, 0);
        set_inputs(use ? 1 : 0, in1, in2);
    }

    // Turn on the snippet's own log before loading it. It is far more
    // forthcoming than any return code -- every refusal names the file, line and
    // function that produced it -- and it is driven by environment variables
    // rather than by the parameter block, so it has to be set up first.
    // When the CUDA driver beside us is the logging proxy rather than ZLUDA
    // itself, point its log at the same directory. Harmless otherwise.
    if (desc.data_path && !GetEnvironmentVariableW(L"NVCUDA_PROXY_LOG", nullptr, 0)) {
        std::wstring proxy_log(desc.data_path);
        if (!proxy_log.empty() && proxy_log.back() != L'\\' && proxy_log.back() != L'/')
            proxy_log += L'\\';
        proxy_log += L"nvcuda_proxy.log";
        SetEnvironmentVariableW(L"NVCUDA_PROXY_LOG", proxy_log.c_str());
    }

    SetEnvironmentVariableW(L"__NGX_LOG_LEVEL", L"3");
    SetEnvironmentVariableW(L"__NGX_ENABLE_OVERRIDE_LOG_PATH", L"1");
    if (desc.data_path) SetEnvironmentVariableW(L"__NGX_LOG_PATH_OVERRIDE", desc.data_path);

    g.snippet_path = desc.dlss_dll_path ? desc.dlss_dll_path : L"";
    report_prior_snippet(L"nvngx_dlssnr.dll");
    report_prior_snippet(L"nvngx_dlss.dll");
    if (const char *missing = g.ngx_load(desc.dlss_dll_path)) {
        set_error("the NGX runtime could not load %s", missing);
        return false;
    }
    {
        // The snippet too: a game that ships DLSS already has one of these
        // loaded, and it will not be the one sitting beside the addon.
        const wchar_t *base = wcsrchr(desc.dlss_dll_path, L'\\');
        report_module(desc.dlss_dll_path, GetModuleHandleW(base ? base + 1 : desc.dlss_dll_path));
    }

    // The snippet aborts the whole process on a fatal error rather than
    // returning, so its log is the only way to see why. Wire it to a sink by
    // default -- the messages go to the debugger output, which ReShade's log
    // and any attached debugger both pick up.
    if (!ngx_ok(g.ngx_init_ext(desc.application_id, desc.data_path, NVSDK_NGX_Version_API,
                               (unsigned long long)(void *)&ngx_log_sink, desc.log_level,
                               g.ctx),
                "NVSDK_NGX_CUDA_Init_Ext"))
        return false;

    // Diagnostic: register a foreign device the way another user of the same
    // snippet in the process does. CreateFeature picks a device by itself only
    // while exactly one is registered, and this is the only way to produce the
    // other case outside a game.
    {
        char buf[8];
        if (GetEnvironmentVariableA("DLSS_DOUBLE_INIT", buf, sizeof buf) > 0) {
            auto foreign = (NVSDK_NGX_Result (*)(unsigned long long, const wchar_t *,
                                                 NVSDK_NGX_Version))
                GetProcAddress(g.ngx, "ngxrt_register_foreign_device");
            const NVSDK_NGX_Result r =
                foreign ? foreign(desc.application_id, desc.data_path, NVSDK_NGX_Version_API)
                        : NVSDK_NGX_Result_Fail;
            fprintf(stderr, "  [diag] foreign device registered -> 0x%08X\n", r);
            fflush(stderr);
        }
    }

    // NGX allocates the parameter block; PopulateParameters_Impl fills in the
    // capability entries the feature needs.
    g.params = nullptr;
    if (!ngx_ok(g.ngx_populate(&g.params), "NVSDK_NGX_CUDA_PopulateParameters_Impl"))
        return false;
    if (!g.params) {
        set_error("NGX returned a null parameter block");
        return false;
    }

    if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        set_error("CreateFence failed");
        return false;
    }
    g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&g.allocator))) ||
        FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr,
                                           IID_PPV_ARGS(&g.cmd)))) {
        set_error("could not create the copy command list");
        return false;
    }
    g.cmd->Close();

    g.initialized = true;
    return true;
}

// A snippet implements one feature and, as the id sweep showed, does not check
// the id it is given -- GetScratchBufferSize accepts every value from 0 to 64.
// The real selection is which DLL got loaded. SuperSampling is passed for both
// because it is the one id known to be accepted, and DLSS_FEATURE_ID is there
// for the case where a build turns out to care.
NVSDK_NGX_Feature feature_id(Feature) {
    char buf[16];
    if (GetEnvironmentVariableA("DLSS_FEATURE_ID", buf, sizeof buf) > 0)
        return (NVSDK_NGX_Feature)atoi(buf);
    return NVSDK_NGX_Feature_SuperSampling;
}

void set_create_params_sr(const FeatureDesc &desc) {
    g.params->Set(ngx_param::Width, desc.render_width);
    g.params->Set(ngx_param::Height, desc.render_height);
    g.params->Set(ngx_param::OutWidth, desc.output_width);
    g.params->Set(ngx_param::OutHeight, desc.output_height);
    g.params->Set(ngx_param::PerfQualityValue, desc.perf_quality);
    g.params->Set(ngx_param::FeatureCreateFlags, desc.create_flags);
    g.params->Set(ngx_param::CreationNodeMask, 1u);
    g.params->Set(ngx_param::VisibilityNodeMask, 1u);
}

// Neural rendering shares no parameter name with super resolution, so this is a
// separate set rather than a variation on one. It publishes both spellings of
// the output size that the snippet's strings contain -- DLSSNR.OutputWidth and
// DLSSNR.Output.Width -- because which one a given build reads is not knowable
// from the strings alone, and setting both costs nothing.
void set_create_params_nr(const FeatureDesc &desc) {
    g.params->Set(nr_param::Enabled, 1);
    g.params->Set(nr_param::Width, desc.render_width);
    g.params->Set(nr_param::Height, desc.render_height);
    g.params->Set(nr_param::InputWidth, desc.render_width);
    g.params->Set(nr_param::InputHeight, desc.render_height);
    g.params->Set(nr_param::OutputWidth, desc.output_width);
    g.params->Set(nr_param::OutputHeight, desc.output_height);
    g.params->Set("DLSSNR.Output.Width", desc.output_width);
    g.params->Set("DLSSNR.Output.Height", desc.output_height);
    // Upscaling is a separate switch here: the network alters the image whether
    // or not it also changes its size, and altering it is the point.
    const bool upscaling = desc.output_width != desc.render_width ||
                           desc.output_height != desc.render_height;
    g.params->Set(nr_param::Upscaling, upscaling ? 1 : 0);
    g.params->Set(nr_param::ScalingRatio,
                  upscaling ? (float)desc.output_width / (float)desc.render_width : 1.0f);
    // Zero means "leave the snippet's own default", which is what it picks when
    // the hint is absent.
    if (desc.neural.render_preset != 0)
        g.params->Set(nr_param::RenderPreset, desc.neural.render_preset);
    g.params->Set(ngx_param::CreationNodeMask, 1u);
    g.params->Set(ngx_param::VisibilityNodeMask, 1u);
}

// The per-frame half of the neural rendering set: the four buffers, and the
// controls over the effect.
void set_frame_params_nr(const FeatureDesc &create, const NeuralRenderingDesc &nr,
                         const FrameDesc &frame) {
    g.params->Set(nr_param::Color, resource_handle(g.color));
    g.params->Set(nr_param::Depth, resource_handle(g.depth));
    g.params->Set(nr_param::MVec, resource_handle(g.motion));
    g.params->Set(nr_param::Output, resource_handle(g.output));
    // The network alters the finished image, so it also asks for the buffer that
    // image lives in. Here that is the same texture it writes to; a caller with
    // a real frame would pass the swapchain's.
    g.params->Set(nr_param::Backbuffer, resource_handle(g.output));

    g.params->Set(nr_param::MVecScaleX, frame.mv_scale_x * nr.mv_scale_multiplier_x);
    g.params->Set(nr_param::MVecScaleY, frame.mv_scale_y * nr.mv_scale_multiplier_y);
    // Always set: an absent value is indistinguishable from a rejected one on
    // the snippet's side. Convention 0 means "as the frame says", which for the
    // self test is plain depth.
    g.params->Set(nr_param::DepthInverted, nr.depth_convention == 2 ? 1 : 0);
    g.params->Set(nr_param::Reset, frame.reset_accumulation ? 1 : 0);

    g.params->Set("DLSSNR.ColorSubrectBaseX", 0u);
    g.params->Set("DLSSNR.ColorSubrectBaseY", 0u);
    g.params->Set("DLSSNR.ColorSubrectWidth", create.render_width);
    g.params->Set("DLSSNR.ColorSubrectHeight", create.render_height);
    g.params->Set("DLSSNR.DepthSubrectBaseX", 0u);
    g.params->Set("DLSSNR.DepthSubrectBaseY", 0u);
    g.params->Set("DLSSNR.DepthSubrectWidth", create.render_width);
    g.params->Set("DLSSNR.DepthSubrectHeight", create.render_height);
    g.params->Set("DLSSNR.MVecSubrectBaseX", 0u);
    g.params->Set("DLSSNR.MVecSubrectBaseY", 0u);
    g.params->Set("DLSSNR.MVecSubrectWidth", create.render_width);
    g.params->Set("DLSSNR.MVecSubrectHeight", create.render_height);
    g.params->Set("DLSSNR.OutputSubrectBaseX", 0u);
    g.params->Set("DLSSNR.OutputSubrectBaseY", 0u);
    g.params->Set("DLSSNR.OutputSubrectWidth", create.output_width);
    g.params->Set("DLSSNR.OutputSubrectHeight", create.output_height);
    // Supplying a resource makes the network ask for its geometry as well; the
    // backbuffer covers the whole output.
    g.params->Set("DLSSNR.BackbufferSubrectBaseX", 0u);
    g.params->Set("DLSSNR.BackbufferSubrectBaseY", 0u);
    g.params->Set("DLSSNR.BackbufferSubrectWidth", create.output_width);
    g.params->Set("DLSSNR.BackbufferSubrectHeight", create.output_height);

    g.params->Set(nr_param::Style, nr.style);
    g.params->Set(nr_param::Intensity, nr.intensity);
    g.params->Set(nr_param::GlobalToneStrength, nr.global_tone_strength);
    g.params->Set(nr_param::LocalToneStrength, nr.local_tone_strength);
    g.params->Set(nr_param::LocalStructureStrength, nr.local_structure_strength);
    g.params->Set(nr_param::SkinStructureStrength, nr.skin_structure_strength);
    // Off, whatever the caller asked for, until this layer actually supplies a
    // user interface texture.
    //
    // The correction composites the finished image against DLSSNR.UI and
    // DLSSNR.UIAlpha. We have never set either -- the parameter trace showed
    // both missing from the first run -- and with the correction on the network
    // composites against nothing and the result collapses to black. That is the
    // whole of the black screen: measured, the same frame comes out at mean
    // luminance 1 with the correction on and 50 with it off, against 50 going
    // in.
    g.params->Set(nr_param::UICorrection, 0);
    g.params->Set(nr_param::UseAutoMask, nr.use_auto_mask ? 1 : 0);
    // Read every frame and, when absent, indistinguishable from a rejected
    // value on the snippet's side. The scaling ratio in particular is read here
    // as well as at creation.
    g.params->Set(nr_param::ScalingRatio,
                  (float)create.output_width / (float)create.render_width);
    g.params->Set("DLSS.Indicator.Invert.X.Axis", 0);
    g.params->Set("DLSS.Indicator.Invert.Y.Axis", 0);
}

bool create_feature(const FeatureDesc &desc) {
    if (!g.initialized) {
        set_error("create_feature called before init");
        return false;
    }
    // DLSS_FORCE_RECREATE_FEATURE=1 skips the reuse below even when nothing
    // about the request changed, so every evaluate() gets a feature -- and
    // whatever internal workspace the snippet allocates for it -- that has
    // never been touched by a previous call in this same process.
    //
    // Diagnostic, added to chase the non-determinism this project has never
    // pinned down (two stable outcomes over repeated identical evaluations,
    // see the note on global_tone_strength/skin_structure_strength in
    // dlss_cuda.h): a run of the enhancer GUI reprocessing one already-loaded
    // image over and over always lands on the same outcome, good or flat,
    // and only loading a *different* image can change which one -- which
    // reuse of this cached feature makes possible, and this variable removes,
    // to see whether that stops the stickiness.
    char force_recreate[8];
    const bool always_recreate =
        GetEnvironmentVariableA("DLSS_FORCE_RECREATE_FEATURE", force_recreate, sizeof force_recreate) >
            0 &&
        force_recreate[0] == '1';
    if (!always_recreate && g.feature && desc.render_width == g.current.render_width &&
        desc.render_height == g.current.render_height &&
        desc.output_width == g.current.output_width &&
        desc.output_height == g.current.output_height &&
        desc.perf_quality == g.current.perf_quality && desc.create_flags == g.current.create_flags)
        return true;

    if (g.feature) {
        g.ngx_release(g.feature);
        g.feature = nullptr;
    }

    // Colour and output are HDR-friendly; depth is single-channel float; motion
    // vectors are two-channel float at render resolution.
    // Colour is written by the conversion pass on the way in, so it needs an
    // unordered access view of its own; a swapchain buffer cannot be copied
    // into it directly, the formats differ.
    if (!make_shared(g.color, desc.render_width, desc.render_height,
                     DXGI_FORMAT_R16G16B16A16_FLOAT, true) ||
        !make_shared(g.depth, desc.render_width, desc.render_height, DXGI_FORMAT_R32_FLOAT, false) ||
        !make_shared(g.motion, desc.render_width, desc.render_height, DXGI_FORMAT_R16G16_FLOAT,
                     false) ||
        !make_shared(g.output, desc.output_width, desc.output_height,
                     DXGI_FORMAT_R16G16B16A16_FLOAT, true))
        return false;

    if (desc.feature == Feature::NeuralRendering)
        set_create_params_nr(desc);
    else
        set_create_params_sr(desc);

    // Render preset. The log shows the snippet defaulting to K, a transformer
    // network; the older presets are convolutional and take a different code
    // path entirely. Selectable by environment so the two families can be
    // compared without a rebuild -- if one evaluates and the other does not,
    // that localises the failure enormously.
    {
        char buf[16];
        if (GetEnvironmentVariableA("DLSS_PRESET", buf, sizeof buf) > 0) {
            unsigned preset = (unsigned)atoi(buf);
            for (const char *name : {"DLSS.Hint.Render.Preset.DLAA",
                                     "DLSS.Hint.Render.Preset.Quality",
                                     "DLSS.Hint.Render.Preset.Balanced",
                                     "DLSS.Hint.Render.Preset.Performance",
                                     "DLSS.Hint.Render.Preset.UltraPerformance",
                                     "DLSS.Hint.Render.Preset.UltraQuality"}) {
                g.params->Set(name, preset);
            }
        }
    }

    // How much device memory is left, before the snippet asks for its own. A
    // network that will not be created is often a network that was not given
    // room, and that refusal comes back as a generic failure.
    if (g.cu.cuMemGetInfo) {
        size_t free_bytes = 0, total_bytes = 0;
        if (g.cu.cuMemGetInfo(&free_bytes, &total_bytes) == CUDA_SUCCESS) {
            char line[256];
            snprintf(line, sizeof line, "[dlss-cuda] device memory: %zu MB free of %zu MB\n",
                     free_bytes / (1024 * 1024), total_bytes / (1024 * 1024));
            OutputDebugStringA(line);
            fputs(line, stderr);
            if (g_reshade_log) g_reshade_log(line);
        }
    }

    report_device_count("before CreateFeature");
    const NVSDK_NGX_Result create_result =
        g.ngx_create(feature_id(desc.feature), g.params, &g.feature);
    // Which branch the runtime actually took. A fix that lives in nvngx.dll does
    // nothing if an older copy of that file is the one beside the addon, and the
    // return code alone cannot tell the two apart.
    if (auto path = (int (*)(void))GetProcAddress(g.ngx, "ngxrt_create_path")) {
        auto build = (const char *(*)(void))GetProcAddress(g.ngx, "ngxrt_build_id");
        static const char *const kPaths[] = {"plain CreateFeature",
                                             "CreateFeature1 with input params",
                                             "CreateFeature1 naming our device"};
        const int which = path();
        char line[256];
        snprintf(line, sizeof line, "[dlss-cuda] NGX runtime built %s, create path: %s\n",
                 build ? build() : "?",
                 (which >= 0 && which <= 2) ? kPaths[which] : "unknown");
        OutputDebugStringA(line);
        fputs(line, stderr);
        if (g_reshade_log) g_reshade_log(line);
    } else {
        const char *line = "[dlss-cuda] WARNING: the nvngx.dll beside the addon predates the "
                           "device-naming fix; replace it with the one shipped alongside\n";
        OutputDebugStringA(line);
        fputs(line, stderr);
        if (g_reshade_log) g_reshade_log(line);
    }
    if (!ngx_ok(create_result, "NVSDK_NGX_CUDA_CreateFeature")) {
        report_snippet_compression(g.snippet_path.c_str());
        return false;
    }

    g.current = desc;
    return true;
}

// Waits for the work the evaluation queued and reports what came of it. Both
// evaluation paths end here, so what one of them proves the other does too.
static bool finish_evaluation() {
    const CUresult first = g.cu.cuCtxSynchronize();
    if (first != CUDA_SUCCESS) {
        // A device error is sticky, so a second synchronise that succeeds means
        // the first code did not come from the work the network queued.
        const CUresult second = g.cu.cuCtxSynchronize();
        fprintf(stderr, "  [sync] first=%d second=%d\n", first, second);
        fflush(stderr);
    }

    // What actually matters: whether the output texture holds an image. The
    // return codes above say the calls were accepted; only the pixels say the
    // network ran.
    //
    // Once, not once a frame: reading rows back costs real time and the answer
    // does not change between frames. What it separates is a network that
    // produced nothing from a result produced and then lost on its way to the
    // screen.
    static bool reported = false;
    if (!reported && g.cu.cuMemcpy2D && g.output.level0) {
        reported = true;
        CUDA_ARRAY_DESCRIPTOR ad{};
        if (g.cu.cuArrayGetDescriptor(&ad, g.output.level0) == CUDA_SUCCESS) {
            const size_t bytes_per_texel =
                (ad.Format == CU_AD_FORMAT_FLOAT ? 4u : ad.Format == CU_AD_FORMAT_HALF ? 2u : 1u) *
                ad.NumChannels;
            const size_t row_bytes = ad.Width * bytes_per_texel;
            const size_t rows = ad.Height < 64 ? ad.Height : 64;
            std::vector<unsigned char> host(row_bytes * rows, 0xCD);
            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.srcArray = g.output.level0;
            copy.dstMemoryType = CU_MEMORYTYPE_HOST;
            copy.dstHost = host.data();
            copy.dstPitch = row_bytes;
            copy.WidthInBytes = row_bytes;
            copy.Height = rows;
            const CUresult cr = g.cu.cuMemcpy2D(&copy);
            if (cr != CUDA_SUCCESS) {
                fprintf(stderr, "  [output] read back failed: %d\n", cr);
            } else {
                size_t nonzero = 0;
                for (unsigned char b : host)
                    if (b) ++nonzero;
                char line[224];
                snprintf(line, sizeof line,
                         "[dlss-cuda] network output: %zux%zu, %zu rows read back, %zu of %zu "
                         "bytes non-zero\n", ad.Width, ad.Height, rows, nonzero, host.size());
                fputs(line, stderr);
                if (g_reshade_log) g_reshade_log(line);
            }
            fflush(stderr);
        }
    }
    return cu_ok(first, "cuCtxSynchronize");
}

// The NGX half of an evaluation: bind the arrays, set the per-frame values and
// run. Assumes the shared textures already hold what they should.
static bool evaluate_ngx(const FrameDesc &frame) {
    if (!cu_ok(g.cu.cuCtxSetCurrent(g.ctx), "cuCtxSetCurrent")) return false;

    // DLSS resolves cuSurfObjectCreate, cuTexObjectCreate and
    // cuMipmappedArrayDestroy but imports no external memory of its own, so it
    // takes ownership of array handles handed to it and builds its own surface
    // and texture objects. That is why the mipmapped array is passed here rather
    // than a surface object.
    if (g.current.feature == Feature::NeuralRendering) {
        set_frame_params_nr(g.current, g.current.neural, frame);
        if (!ngx_ok(g.ngx_evaluate(g.feature, g.params), "NVSDK_NGX_CUDA_EvaluateFeature"))
            return false;
        return finish_evaluation();
    }

    g.params->Set(ngx_param::Color, resource_handle(g.color));
    g.params->Set(ngx_param::Depth, resource_handle(g.depth));
    g.params->Set(ngx_param::MotionVectors, resource_handle(g.motion));
    g.params->Set(ngx_param::Output, resource_handle(g.output));

    g.params->Set(ngx_param::JitterOffsetX, frame.jitter_x);
    g.params->Set(ngx_param::JitterOffsetY, frame.jitter_y);
    g.params->Set(ngx_param::MVScaleX, frame.mv_scale_x);
    g.params->Set(ngx_param::MVScaleY, frame.mv_scale_y);
    g.params->Set(ngx_param::Sharpness, frame.sharpness);
    g.params->Set(ngx_param::PreExposure, frame.pre_exposure);
    g.params->Set(ngx_param::Reset, frame.reset_accumulation ? 1 : 0);
    g.params->Set(ngx_param::RenderSubrectWidth, g.current.render_width);
    g.params->Set(ngx_param::RenderSubrectHeight, g.current.render_height);

    // The scalars the evaluation asks for and would otherwise not find. They are
    // documented as optional, but the trace shows every one of them queried, and
    // an absent entry is indistinguishable from a rejected one on the snippet's
    // side. Giving them their neutral values removes that ambiguity.
    g.params->Set("#", 0);
    g.params->Set("MV.Offset.X", 0.0f);
    g.params->Set("MV.Offset.Y", 0.0f);
    g.params->Set("Jitter.Slope.X", 1.0f);
    g.params->Set("Jitter.Slope.Y", 1.0f);
    g.params->Set(ngx_param::ExposureScale, 1.0f);
    g.params->Set("TonemapperType", 0);
    g.params->Set("FrameTimeDeltaInMsec", 16.6f);
    g.params->Set("DLSS.Checkerboard.Jitter.Hack", 0);
    g.params->Set("DLSS.Indicator.Invert.X.Axis", 0);
    g.params->Set("DLSS.Indicator.Invert.Y.Axis", 0);
    g.params->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
    g.params->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
    g.params->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
    g.params->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
    g.params->Set("DLSS.Input.MV.Subrect.Base.X", 0u);
    g.params->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
    g.params->Set("DLSS.Output.Subrect.Base.X", 0u);
    g.params->Set("DLSS.Output.Subrect.Base.Y", 0u);

    // Device errors are sticky, so a synchronise here says whether what the one
    // after the evaluation reports was actually produced by the evaluation.
    if (!cu_ok(g.cu.cuCtxSynchronize(), "cuCtxSynchronize (before evaluation)"))
        return false;
    if (!ngx_ok(g.ngx_evaluate(g.feature, g.params), "NVSDK_NGX_CUDA_EvaluateFeature"))
        return false;

    return finish_evaluation();
}

bool evaluate_selftest() {
    // The parameter trace is a diagnostic, and in a game it would be one line per
    // query per frame. Off unless DLSS_TRACE_PARAMS is set.
    if (auto trace = (void (*)(int))GetProcAddress(g.ngx, "ngxrt_trace_params")) {
        char buf[8];
        trace(GetEnvironmentVariableA("DLSS_TRACE_PARAMS", buf, sizeof buf) > 0 ? 1 : 0);
    }
    if (!g.feature) {
        set_error("evaluate_selftest called before create_feature");
        return false;
    }
    FrameDesc frame{};
    frame.reset_accumulation = true;
    return evaluate_ngx(frame);
}

bool evaluate_backbuffer(void *back_buffer, unsigned width, unsigned height, int format) {
    if (!g.feature) {
        set_error("evaluate_backbuffer called before create_feature");
        return false;
    }
    ID3D12Resource *back = static_cast<ID3D12Resource *>(back_buffer);
    if (!back) {
        set_error("evaluate_backbuffer: no back buffer");
        return false;
    }
    if (!frame_blit::init(g.device)) {
        set_error("%s", frame_blit::last_error());
        return false;
    }

    // In: the swapchain buffer read as a texture, written into our colour
    // through an unordered access view. A present handler is handed the buffer
    // in the present state, and has to give it back that way.
    g.allocator->Reset();
    g.cmd->Reset(g.allocator, nullptr);
    transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(g.color.resource, D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool staged = frame_blit::to_shared(g.cmd, back, g.color.resource, width, height);
    transition(g.color.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
    transition(back, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
    g.cmd->Close();
    ID3D12CommandList *in[] = {g.cmd};
    g.queue->ExecuteCommandLists(1, in);
    if (!staged) {
        set_error("%s", frame_blit::last_error());
        return false;
    }
    if (!flush_and_wait()) return false;

    FrameDesc frame{};
    frame.color = g.color.resource;
    frame.output = g.output.resource;
    if (!evaluate_ngx(frame)) return false;

    // Out: our result drawn over the swapchain buffer. It cannot be a compute
    // pass -- a game creates its swapchain without unordered access -- so this
    // direction is a draw into a render target view.
    g.allocator->Reset();
    g.cmd->Reset(g.allocator, nullptr);
    transition(g.output.resource, D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const bool presented =
        frame_blit::to_backbuffer(g.cmd, g.output.resource, back, width, height, format);
    transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    transition(g.output.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    g.cmd->Close();
    ID3D12CommandList *out[] = {g.cmd};
    g.queue->ExecuteCommandLists(1, out);
    if (!presented) {
        set_error("%s", frame_blit::last_error());
        return false;
    }
    return flush_and_wait();
}

bool evaluate(const FrameDesc &frame) {
    if (!g.feature) {
        set_error("evaluate called before create_feature");
        return false;
    }
    if (!frame.color || !frame.output) {
        set_error("evaluate: colour and output are required");
        return false;
    }

    // Stage the game's buffers into the shared textures.
    g.allocator->Reset();
    g.cmd->Reset(g.allocator, nullptr);
    transition(g.color.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    g.cmd->CopyResource(g.color.resource, frame.color);
    transition(g.color.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    // Depth and motion vectors are optional. A caller that has them supplies
    // them; a caller that does not leaves the staging textures as they are,
    // which means zeroes. The network then has no motion to work from and the
    // result is not what it would be in a game that feeds it properly, but the
    // frame does go through the network, which is the thing being built here.
    if (frame.depth) {
        transition(g.depth.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        g.cmd->CopyResource(g.depth.resource, frame.depth);
        transition(g.depth.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    if (frame.motion_vectors) {
        transition(g.motion.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        g.cmd->CopyResource(g.motion.resource, frame.motion_vectors);
        transition(g.motion.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    g.cmd->Close();
    ID3D12CommandList *lists[] = {g.cmd};
    g.queue->ExecuteCommandLists(1, lists);

    // ponytail: full pipeline stall around the CUDA work. The clean way is a
    // shared D3D12 fence imported with cuImportExternalSemaphore, but ROCm does
    // not implement external semaphore import, so the CPU has to be the
    // rendezvous point. Revisit if HIP ever grows semaphore import.
    if (!flush_and_wait()) return false;

    if (!evaluate_ngx(frame)) return false;

    // And back into the game's output.
    g.allocator->Reset();
    g.cmd->Reset(g.allocator, nullptr);
    transition(g.output.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.cmd->CopyResource(frame.output, g.output.resource);
    transition(g.output.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    g.cmd->Close();
    ID3D12CommandList *back[] = {g.cmd};
    g.queue->ExecuteCommandLists(1, back);

    return true;
}

bool debug_read_shared_colour(void *rows, size_t row_bytes, unsigned row_count) {
    if (!g.cu.cuMemcpy2D || !g.color.level0) {
        set_error("debug_read_shared_colour: nothing to read");
        return false;
    }
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.srcArray = g.color.level0;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = rows;
    copy.dstPitch = row_bytes;
    copy.WidthInBytes = row_bytes;
    copy.Height = row_count;
    return cu_ok(g.cu.cuMemcpy2D(&copy), "cuMemcpy2D (colour read back)");
}

void shutdown() {
    if (g.feature && g.ngx_release) g.ngx_release(g.feature);
    g.feature = nullptr;

    release_shared(g.color);
    release_shared(g.depth);
    release_shared(g.motion);
    release_shared(g.output);

    if (g.ngx_shutdown) g.ngx_shutdown();

    if (g.cmd) g.cmd->Release();
    if (g.allocator) g.allocator->Release();
    if (g.fence) g.fence->Release();
    if (g.fence_event) CloseHandle(g.fence_event);
    if (g.ctx && g.cu.cuCtxDestroy) g.cu.cuCtxDestroy(g.ctx);
    if (g.ngx) FreeLibrary(g.ngx);
    if (g.nvcuda) FreeLibrary(g.nvcuda);
    if (g.nvapi) FreeLibrary(g.nvapi);

    g = State{};
}

} // namespace dlss_cuda
