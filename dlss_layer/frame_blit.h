// Format-converting copies between the game's swapchain and this layer's own
// textures.
//
// The network reads and writes half-float colour, while a swapchain is whatever
// the game chose -- R10G10B10A2_UNORM in the case that drove this code.
// CopyResource cannot bridge that: D3D12 requires identical formats, so a frame
// can only reach the network through a shader.
//
// Two directions, and they are not symmetric. Coming in, the destination is our
// own texture and can carry an unordered access view, so a compute pass does
// it. Going out, the destination is a swapchain buffer, which is created by the
// game and generally allows no unordered access, so that direction has to be a
// draw into a render target view.

#pragma once

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace frame_blit {

// Builds the pipelines and descriptor heaps. Returns false and leaves a message
// in last_error() on failure.
bool init(ID3D12Device *device);

// Swapchain buffer -> our half-float colour texture. The source is read as a
// texture and the destination written through an unordered access view, so the
// destination must have been created with that flag.
bool to_shared(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src, ID3D12Resource *dst,
               unsigned width, unsigned height);

// Our half-float output -> the swapchain buffer, as a draw. The pipeline
// depends on the destination's format, so one is built per format seen.
bool to_backbuffer(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src, ID3D12Resource *dst,
                   unsigned width, unsigned height, int dst_format);

void shutdown();

const char *last_error();

} // namespace frame_blit
