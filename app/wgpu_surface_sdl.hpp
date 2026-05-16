#pragma once

#include <webgpu/webgpu.h>

struct SDL_Window;

// Create a wgpu surface from an SDL3 window using the native window handle
// exposed through SDL_GetWindowProperties. Returns NULL on unsupported platforms
// or if the underlying handles are missing.
WGPUSurface create_wgpu_surface_from_sdl(WGPUInstance instance, SDL_Window* window);
