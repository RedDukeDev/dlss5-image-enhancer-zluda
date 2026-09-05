// One still image through the DLSS 5 neural rendering network.
//
// This is the same path the command line tool uses and the ReShade addon runs
// in a game: it links addon/dlss_cuda.cpp unchanged. Nothing here is a second
// implementation of the DLSS side -- that layer is the one that has been made
// to work, and a rewrite would only be a second thing to get wrong.
//
// Windows only. The network ships as a Windows library; on Linux this runs
// under Wine or Proton like any other Windows program.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace enhancer {

// The controls, with the values the addon starts from.
//
// Global tone and skin structure start at zero. A single run with each at one
// produced a flattened picture, but the evaluation is not reproducible -- the
// same settings give a good frame or a flat one -- so that reading is not
// trustworthy and zero is the cautious choice rather than a measured one.
struct Settings {
    float intensity = 1.0f;
    float global_tone = 0.0f;
    float local_tone = 1.0f;
    float local_structure = 1.0f;
    float skin_structure = 0.0f;
    int style = 0;   // 0 default, 1 natural, 2 cinematic
    int preset = 0;  // 0 leaves the choice to the network
    bool auto_mask = true;
    // Repeats the evaluation on the same picture. The network blends with its
    // own previous output, so for a still image this is the nearest thing to a
    // scene standing still.
    int passes = 1;
};

// Where the pieces are. All of them belong to someone else; the program never
// guesses at a location.
struct Paths {
    std::wstring snippet;     // nvngx_dlssnr.dll
    std::wstring cuda_driver; // nvcuda.dll, or ZLUDA standing in for it
    std::wstring ngx_runtime; // nvngx.dll
    std::wstring nvapi;       // nvapi64.dll
};

// Four half-float channels a pixel, rows packed tight: what the network reads
// and writes, and what the interface converts to and from.
struct Image {
    std::vector<uint16_t> pixels;
    unsigned width = 0;
    unsigned height = 0;

    bool empty() const { return pixels.empty() || width == 0 || height == 0; }
    size_t row_bytes() const { return (size_t)width * 8; }
};

class Processor {
public:
    Processor();
    ~Processor();
    Processor(const Processor &) = delete;
    Processor &operator=(const Processor &) = delete;

    // Brings up Direct3D, the CUDA driver and the network. On anything but a
    // real NVIDIA driver this first translates the network's code in parallel
    // (see precompile.h) rather than leaving it to happen serially, one module
    // at a time, wherever the network first reaches for it. `log`, if given, is
    // called with a line of progress at a time as that runs; it may be called
    // from this thread only, never concurrently.
    bool start(const Paths &paths, std::string &error,
              const std::function<void(const std::string &)> &log = {});
    bool started() const;
    void stop();

    // Runs `in` through the network into `out`. Rebuilds the network if the
    // image size changed since the last call.
    bool process(const Image &in, Image &out, const Settings &settings, std::string &error);

    // Milliseconds the last call spent, and what the CUDA device calls itself.
    double last_ms() const;
    const std::string &device_name() const;

private:
    struct State;
    State *s;
};

} // namespace enhancer
