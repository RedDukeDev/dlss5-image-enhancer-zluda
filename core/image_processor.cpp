#include "image_processor.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <chrono>

#include "dlss_cuda.h"
#include "precompile.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "version.lib")

namespace enhancer {
namespace {

// Whether the driver at this path is NVIDIA's own, as opposed to ZLUDA or the
// diagnostic proxy standing in for it.
//
// Every NVIDIA driver binary carries "NVIDIA Corporation" as the CompanyName
// in its version resource; neither ZLUDA's own build of nvcuda.dll nor the
// proxy in tools/nvcuda_proxy.cpp has a version resource at all, so an absent
// or different CompanyName means it is not the real thing. This is what
// decides whether the network's code needs translating at all: on real
// hardware the driver already carries machine code for it and there is
// nothing to precompile.
bool is_real_nvidia_driver(const std::wstring &path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return false;
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), handle, size, buffer.data())) return false;

    // The language/codepage a version resource was built with is not fixed, so
    // ask the block for the one it actually has rather than guessing 040904B0.
    struct LangCodepage {
        WORD language;
        WORD codepage;
    } *translations = nullptr;
    UINT translations_bytes = 0;
    if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void **>(&translations), &translations_bytes) ||
        translations_bytes < sizeof(LangCodepage)) {
        return false;
    }

    wchar_t query[64];
    swprintf(query, 64, L"\\StringFileInfo\\%04x%04x\\CompanyName", translations[0].language,
             translations[0].codepage);
    wchar_t *company = nullptr;
    UINT company_len = 0;
    if (!VerQueryValueW(buffer.data(), query, reinterpret_cast<void **>(&company),
                        &company_len) ||
        !company) {
        return false;
    }
    return wcsstr(company, L"NVIDIA") != nullptr;
}

// A texture copy moves aligned rows, and the rows coming from an image file are
// not aligned, so everything goes through a staging buffer.
UINT aligned_pitch(UINT bytes) {
    constexpr UINT alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    return (bytes + alignment - 1) & ~(alignment - 1);
}

ID3D12Resource *make_texture(ID3D12Device *device, UINT width, UINT height, bool writable) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    if (writable) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *out = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out));
    return out;
}

ID3D12Resource *make_buffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE type,
                            D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *out = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                    IID_PPV_ARGS(&out));
    return out;
}

void release(IUnknown *&object) {
    if (object) object->Release();
    object = nullptr;
}

} // namespace

struct Processor::State {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *cmd = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;

    // Rebuilt whenever the image size changes.
    ID3D12Resource *colour = nullptr;
    ID3D12Resource *result = nullptr;
    ID3D12Resource *upload = nullptr;
    ID3D12Resource *readback = nullptr;
    unsigned width = 0, height = 0;

    bool started = false;
    // Whether start() has ever run, successfully or not.
    bool attempted = false;
    double last_ms = 0.0;
    std::string device_name;

    void wait() {
        queue->Signal(fence, ++fence_value);
        if (fence->GetCompletedValue() < fence_value) {
            fence->SetEventOnCompletion(fence_value, fence_event);
            WaitForSingleObject(fence_event, INFINITE);
        }
    }

    void release_images() {
        release(reinterpret_cast<IUnknown *&>(colour));
        release(reinterpret_cast<IUnknown *&>(result));
        release(reinterpret_cast<IUnknown *&>(upload));
        release(reinterpret_cast<IUnknown *&>(readback));
        width = height = 0;
    }
};

Processor::Processor() : s(new State) {}
Processor::~Processor() {
    stop();
    delete s;
}

bool Processor::started() const { return s->started; }
double Processor::last_ms() const { return s->last_ms; }
const std::string &Processor::device_name() const { return s->device_name; }

bool Processor::start(const Paths &paths, std::string &error,
                      const std::function<void(const std::string &)> &log) {
    if (s->started) return true;

    // A previous attempt that failed left the network loaded and, depending on
    // how far it got, initialised. Initialising it twice in one process is
    // refused with 0xBAD00002, which then hides whatever went wrong the first
    // time. So a retry starts from a clean state rather than on top of the
    // wreckage.
    if (s->attempted) {
        dlss_cuda::shutdown();
        s->release_images();
    }
    s->attempted = true;

    // A device on the first hardware adapter. No window and no swapchain: this
    // program never presents anything, it only needs Direct3D because that is
    // what the DLSS layer shares its images through.
    IDXGIFactory4 *factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        error = "DXGI could not be started";
        return false;
    }
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&s->device)))) {
            adapter->Release();
            break;
        }
        adapter->Release();
    }
    factory->Release();
    if (!s->device) {
        error = "no Direct3D 12 device could be created";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(s->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&s->queue))) ||
        FAILED(s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&s->allocator))) ||
        FAILED(s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->allocator,
                                            nullptr, IID_PPV_ARGS(&s->cmd))) ||
        FAILED(s->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->fence)))) {
        error = "the Direct3D command objects could not be created";
        return false;
    }
    s->cmd->Close();
    s->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // The network loads the CUDA driver itself, by the name nvcuda.dll, whatever
    // this program was pointed at. Two things follow.
    //
    // The file has to actually be called nvcuda.dll. Pointing at ZLUDA under its
    // build name, zluda_real.dll, satisfies this program and then the network
    // fails on its own with 0xBAD00002 out of Init -- a code that says nothing.
    // Its log is the only place the reason appears: "failed to load cuda DLL".
    //
    // An empty field is not a wrong name: it means "resolve nvcuda.dll the
    // normal way", which is what a real NVIDIA machine wants -- the driver
    // installs into System32, on the standard search path, and does not need
    // to be found by hand. Only a non-empty field is held to the naming rule.
    if (!paths.cuda_driver.empty()) {
        const size_t slash = paths.cuda_driver.find_last_of(L"/" L"\\");
        const std::wstring name = slash == std::wstring::npos
                                      ? paths.cuda_driver
                                      : paths.cuda_driver.substr(slash + 1);
        if (_wcsicmp(name.c_str(), L"nvcuda.dll") != 0) {
            error = "the CUDA driver has to be a file named nvcuda.dll. The network loads it "
                    "by that name on its own, whatever this program is pointed at, so any "
                    "other name fails inside the network with nothing to explain it. Point at "
                    "ZLUDA's nvcuda.dll rather than at zluda_real.dll, or leave this blank to "
                    "use the system's own on a real NVIDIA machine.";
            return false;
        }
        // And its directory has to be searchable, both for the network's own
        // load and for a proxy driver that forwards to a library beside it.
        if (slash != std::wstring::npos)
            SetDllDirectoryW(paths.cuda_driver.substr(0, slash).c_str());
    }

    // On anything other than a real NVIDIA driver, the network's code has to be
    // translated before it can run, and translation inside the network is
    // serial -- one module at a time, tens of minutes for the largest one. Do
    // it here instead, in parallel, before that path is ever reached.
    //
    // No attempt is made to tell a cold cache from a warm one, and it does not
    // need one: measured, with ZLUDA's cache actually holding on to what it
    // is given (see the busy_timeout fix in zluda_cache -- without it, up to
    // sixteen translations finishing near enough together mostly lost the
    // race to save their own result, so the *next* run found nothing there
    // either, forever), a fully warm run of all fifteen modules of this
    // network took 2.6 seconds. That is the cost paid every time this program
    // starts, against tens of minutes the one time a module is actually
    // missing -- not worth a per-module check to shave off.
    //
    // An empty field is checked first and on its own, ahead of asking what
    // driver it names: NVIDIA mode leaves this field empty on purpose (see
    // set_nvidia_mode in the GUI), and precompile has no path to hand its
    // spawned copies in that case regardless of which GPU is underneath --
    // is_real_nvidia_driver(L"") fails to open anything and, on that empty
    // failure, reports "not NVIDIA", which used to send this down the ZLUDA
    // path by mistake on real hardware too.
    if (!paths.cuda_driver.empty() && !is_real_nvidia_driver(paths.cuda_driver)) {
        std::string precompile_error;
        const bool ok = precompile(
            paths.snippet, paths.cuda_driver, 0,
            [&log](const Progress &progress) {
                if (log) log(progress.message);
            },
            precompile_error);
        // Not fatal: whatever did not get translated here still gets translated
        // the slow way when the network reaches for it, just as it always did.
        if (!ok && log) log("precompile: " + precompile_error);
    }

    // The network refuses anything below a Blackwell part, and asks NVAPI what
    // this is. The stand-in NVAPI reads this and answers accordingly, so it has
    // to be set before the driver is loaded.
    SetEnvironmentVariableW(L"ZLUDA_NVAPI_GPU_ARCH", L"0x1B0");

    // An empty field becomes a null pointer, not a pointer to an empty string:
    // dlss_cuda.cpp's own fallback for the driver and the NGX runtime only
    // triggers on null ("load nvcuda.dll / nvngx.dll by bare name and let
    // Windows' own search find the real one"; nvapi already checks for both).
    // A pointer to "" bypassed that fallback and failed loading an empty path.
    const auto or_null = [](const std::wstring &path) {
        return path.empty() ? nullptr : path.c_str();
    };

    dlss_cuda::InitDesc init{};
    init.device = s->device;
    init.queue = s->queue;
    init.data_path = L".";
    init.application_id = 0;
    init.dlss_dll_path = paths.snippet.c_str();
    init.nvcuda_dll_path = or_null(paths.cuda_driver);
    init.ngx_runtime_path = or_null(paths.ngx_runtime);
    init.nvapi_dll_path = or_null(paths.nvapi);
    if (!dlss_cuda::init(init)) {
        error = dlss_cuda::last_error();
        return false;
    }

    s->started = true;
    return true;
}

bool Processor::process(const Image &in, Image &out, const Settings &settings,
                        std::string &error) {
    if (!s->started) {
        error = "the DLSS layer has not been started";
        return false;
    }
    if (in.empty()) {
        error = "no image to work on";
        return false;
    }

    const auto began = std::chrono::steady_clock::now();
    const UINT row_bytes = in.width * 8;
    const UINT padded = aligned_pitch(row_bytes);

    if (in.width != s->width || in.height != s->height) {
        s->release_images();
        s->colour = make_texture(s->device, in.width, in.height, false);
        s->result = make_texture(s->device, in.width, in.height, true);
        s->upload = make_buffer(s->device, (UINT64)padded * in.height, D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
        s->readback = make_buffer(s->device, (UINT64)padded * in.height, D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
        if (!s->colour || !s->result || !s->upload || !s->readback) {
            error = "the working images could not be created";
            return false;
        }
        s->width = in.width;
        s->height = in.height;
    }

    // The network alters an image without resizing it and refuses any other
    // arrangement, so input and output share one size.
    dlss_cuda::FeatureDesc feature{};
    feature.feature = dlss_cuda::Feature::NeuralRendering;
    feature.output_width = feature.render_width = in.width;
    feature.output_height = feature.render_height = in.height;
    feature.perf_quality = 2;
    feature.neural.intensity = settings.intensity;
    feature.neural.global_tone_strength = settings.global_tone;
    feature.neural.local_tone_strength = settings.local_tone;
    feature.neural.local_structure_strength = settings.local_structure;
    feature.neural.skin_structure_strength = settings.skin_structure;
    feature.neural.style = settings.style;
    feature.neural.render_preset = settings.preset;
    feature.neural.use_auto_mask = settings.auto_mask;
    if (!dlss_cuda::create_feature(feature)) {
        error = dlss_cuda::last_error();
        return false;
    }

    {
        unsigned char *mapped = nullptr;
        D3D12_RANGE nothing{0, 0};
        s->upload->Map(0, &nothing, (void **)&mapped);
        for (unsigned y = 0; y < in.height; ++y)
            memcpy(mapped + (size_t)y * padded,
                   (const unsigned char *)in.pixels.data() + (size_t)y * row_bytes, row_bytes);
        s->upload->Unmap(0, nullptr);
    }

    D3D12_TEXTURE_COPY_LOCATION into{};
    into.pResource = s->colour;
    into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = s->upload;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    from.PlacedFootprint.Footprint.Width = in.width;
    from.PlacedFootprint.Footprint.Height = in.height;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = padded;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s->colour;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    s->allocator->Reset();
    s->cmd->Reset(s->allocator, nullptr);
    s->cmd->ResourceBarrier(1, &barrier);
    s->cmd->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    s->cmd->ResourceBarrier(1, &barrier);
    s->cmd->Close();
    ID3D12CommandList *lists[] = {s->cmd};
    s->queue->ExecuteCommandLists(1, lists);
    s->wait();

    dlss_cuda::FrameDesc frame{};
    frame.color = s->colour;
    frame.output = s->result;
    // Depth and motion vectors are left out: a still image has neither, and the
    // layer accepts their absence.
    //
    // The network blends with its own previous output, and that history starts
    // black, so the first pass has to say it is a first frame. Repeating after
    // that is what lets the blend settle on a picture that is not moving.
    frame.reset_accumulation = true;
    const int passes = settings.passes < 1 ? 1 : settings.passes;
    for (int pass = 0; pass < passes; ++pass) {
        if (!dlss_cuda::evaluate(frame)) {
            error = dlss_cuda::last_error();
            return false;
        }
        frame.reset_accumulation = false;
    }

    D3D12_RESOURCE_BARRIER back{};
    back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    back.Transition.pResource = s->result;
    back.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    back.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    back.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = s->result;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = s->readback;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    target.PlacedFootprint.Footprint.Width = in.width;
    target.PlacedFootprint.Footprint.Height = in.height;
    target.PlacedFootprint.Footprint.Depth = 1;
    target.PlacedFootprint.Footprint.RowPitch = padded;

    s->allocator->Reset();
    s->cmd->Reset(s->allocator, nullptr);
    s->cmd->ResourceBarrier(1, &back);
    s->cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    std::swap(back.Transition.StateBefore, back.Transition.StateAfter);
    s->cmd->ResourceBarrier(1, &back);
    s->cmd->Close();
    s->queue->ExecuteCommandLists(1, lists);
    s->wait();

    out.width = in.width;
    out.height = in.height;
    out.pixels.resize((size_t)in.width * in.height * 4);
    {
        unsigned char *mapped = nullptr;
        D3D12_RANGE whole{0, (SIZE_T)padded * in.height};
        if (FAILED(s->readback->Map(0, &whole, (void **)&mapped))) {
            error = "the result could not be read back";
            return false;
        }
        for (unsigned y = 0; y < in.height; ++y)
            memcpy((unsigned char *)out.pixels.data() + (size_t)y * row_bytes,
                   mapped + (size_t)y * padded, row_bytes);
        s->readback->Unmap(0, nullptr);
    }

    s->last_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    return true;
}

void Processor::stop() {
    if (s->started) dlss_cuda::shutdown();
    s->started = false;
    s->attempted = false;
    s->release_images();
    if (s->fence_event) CloseHandle(s->fence_event);
    s->fence_event = nullptr;
    release(reinterpret_cast<IUnknown *&>(s->cmd));
    release(reinterpret_cast<IUnknown *&>(s->allocator));
    release(reinterpret_cast<IUnknown *&>(s->fence));
    release(reinterpret_cast<IUnknown *&>(s->queue));
    release(reinterpret_cast<IUnknown *&>(s->device));
}

} // namespace enhancer
