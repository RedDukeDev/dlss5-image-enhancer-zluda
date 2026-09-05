// DLSS over the NGX CUDA backend, for GPUs that are not NVIDIA.
//
// The graphics NGX backends (D3D11/D3D12/Vulkan) route through the NVIDIA
// driver and are unusable here. The CUDA backend only needs a CUDA driver, and
// nvngx_dlss.dll resolves nvcuda.dll dynamically -- its import table lists only
// ADVAPI32, KERNEL32, USER32 and VERSION -- so a stand-in CUDA driver such as
// ZLUDA can serve it.
//
// This layer owns everything between a D3D12 frame and DLSS: shared textures,
// the CUDA import of them, and the NGX calls. It deliberately does not know
// where the game's buffers come from; the caller (a ReShade addon, RenoDX, or a
// test harness) passes them in.

#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12CommandQueue;

namespace dlss_cuda {

struct InitDesc {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    // Directory NGX may write logs and caches into. Must exist.
    const wchar_t *data_path = L".";
    unsigned long long application_id = 0;
    // Path to nvngx_dlss.dll. Null means the normal DLL search order.
    const wchar_t *dlss_dll_path = nullptr;
    // Path to our NGX runtime, nvngx.dll. Null means the normal search order.
    // The snippet only accepts calls coming from a module with that name, so
    // this is not optional plumbing -- see ngx_runtime.cpp.
    const wchar_t *ngx_runtime_path = nullptr;
    // Path to ZLUDA's nvapi64.dll. The snippet decides its GPU architecture
    // through NVAPI and loads it with LOAD_LIBRARY_SEARCH_SYSTEM32, so a copy
    // sitting next to the game is never found. Pre-loading it by full path puts
    // it in the process under the right base name, and the loader then hands the
    // snippet the module that is already there. Null skips this, and the snippet
    // falls back to reporting a Kepler GPU.
    const wchar_t *nvapi_dll_path = nullptr;

    // Path to the CUDA driver. Null means "nvcuda.dll".
    const wchar_t *nvcuda_dll_path = nullptr;
    // NGX logging verbosity: 0 off, 1 on, 2 verbose. Worth leaving on, because
    // the snippet reports fatal errors to its log and then terminates the whole
    // process instead of returning a result.
    unsigned log_level = 2;
};

// Which network to run. They are different snippet DLLs and share no parameter
// name, so the choice reaches into every NGX call this layer makes.
enum class Feature {
    // nvngx_dlss.dll -- super resolution.
    SuperResolution,
    // nvngx_dlssnr.dll -- neural rendering: it alters the image rather than
    // upscaling it. Its inputs are the same four buffers super resolution takes,
    // with no G-buffer of any kind, which is what makes it reachable from a
    // generic ReShade addon.
    NeuralRendering,
};

// The neural rendering controls. All of them are neutral at zero except
// intensity; the snippet clamps out of range values itself.
struct NeuralRenderingDesc {
    int32_t style = 0;
    float intensity = 1.0f;
    // The five strengths the network takes. Ours all started at zero, which is
    // why its effect was barely visible: it was being asked for none of it.
    // Local tone and local structure at one give a clear, sane improvement.
    //
    // Global tone and skin structure are left at zero, but on weaker grounds
    // than the others: single runs with each at one produced a flattened frame,
    // and only afterwards did repeated runs show the evaluation itself is not
    // reproducible -- the same parameters give a good frame or a flat one,
    // measured five to three over eight identical runs. Those two readings were
    // therefore single samples of a coin flip and prove nothing about the
    // parameters. Zero is kept as the cautious choice until the race behind the
    // non-determinism is fixed and the measurement can be repeated honestly.
    float global_tone_strength = 0.0f;
    float local_tone_strength = 1.0f;
    float local_structure_strength = 1.0f;
    float skin_structure_strength = 0.0f;
    // Leaves interface elements alone, which otherwise get altered along with
    // the rest of the frame.
    bool ui_correction = true;
    // Lets the network derive its own mask instead of taking one.
    bool use_auto_mask = true;
    // Which trained network to use. Zero leaves the snippet's own default.
    int32_t render_preset = 0;

    // Guide overrides. The network is told how the game's depth and motion
    // vectors are laid out; when the game's own convention is wrong or unknown,
    // these say it explicitly. 0 keeps whatever the caller passed in the frame,
    // 1 forces normal depth, 2 forces inverted.
    int32_t depth_convention = 0;
    // Multipliers on the frame's motion vector scale, not replacements: a game
    // whose vectors are in a different unit is corrected here without losing
    // the per-frame value.
    float mv_scale_multiplier_x = 1.0f;
    float mv_scale_multiplier_y = 1.0f;
};

struct FeatureDesc {
    Feature feature = Feature::SuperResolution;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    int32_t perf_quality = 2;  // NVSDK_NGX_PerfQuality_Value_MaxQuality
    uint32_t create_flags = 0; // NVSDK_NGX_DLSS_Feature_Flags_*
    // Only read when feature is NeuralRendering.
    NeuralRenderingDesc neural{};
};

struct FrameDesc {
    // Game resources. They are copied into this layer's own shared textures,
    // so they need no special creation flags.
    ID3D12Resource *color = nullptr;
    ID3D12Resource *depth = nullptr;
    ID3D12Resource *motion_vectors = nullptr;
    ID3D12Resource *output = nullptr;

    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float mv_scale_x = 1.0f;
    float mv_scale_y = 1.0f;
    float sharpness = 0.0f;
    float pre_exposure = 1.0f;
    bool reset_accumulation = false;
};

// Loads the CUDA driver and nvngx_dlss.dll, creates a CUDA context and calls
// NVSDK_NGX_CUDA_Init_Ext. Returns false and leaves last_error() set on failure.
bool init(const InitDesc &desc);

// Creates (or recreates, if the sizes changed) the DLSS feature and the shared
// textures behind it.
bool create_feature(const FeatureDesc &desc);

// Runs one DLSS evaluation. Stalls the D3D12 queue around the CUDA work; see
// the note on synchronisation in dlss_cuda.cpp.
bool evaluate(const FrameDesc &frame);

// Runs one evaluation over a swapchain buffer, converting into and out of the
// half-float colour the network works in: the buffer goes in as colour and the
// result comes back over it. Depth and motion vectors are left at zero, so what
// comes out is the network's answer without any motion to work from.
//
// `back_buffer` is an ID3D12Resource, `format` its DXGI_FORMAT.
bool evaluate_backbuffer(void *back_buffer, unsigned width, unsigned height, int format);

// Runs one evaluation against this layer's own staging textures, without
// touching any game resource. Nothing is copied in or out: it exists to prove
// that init -> create_feature -> evaluate completes on this GPU.
bool evaluate_selftest();

// Reads back what CUDA sees in the colour texture the network reads from.
//
// Every check so far ran against zeroed textures, where a broken import from
// D3D12 to CUDA is invisible: black in, black out. This is the one way to tell
// whether the picture actually crosses that boundary.
//
// `rows` receives `row_count` rows of `row_bytes` bytes, half-float RGBA.
bool debug_read_shared_colour(void *rows, size_t row_bytes, unsigned row_count);

void shutdown();

// Routes NGX's own log lines somewhere the user can see them. NGX reports fatal
// conditions there and then terminates, so losing them loses the explanation.
void set_log_sink(void (*sink)(const char *));

// The CUDA context this layer created, or null before init. Diagnostics need it
// to tell whether the snippet keyed its internal state by the same context we
// handed to Init.
void *cuda_context();

// Takes device memory and never gives it back. Diagnostic only: it makes an
// empty harness as short of room as a running game is.
bool hold_device_memory(size_t bytes);

// Human-readable description of the last failure. Never null.
const char *last_error();

} // namespace dlss_cuda
