#include "wgpu_surface_sdl.hpp"

#include <SDL3/SDL.h>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  // Metal surface creation requires bridging to Objective-C; add when extending to mac.
#elif defined(__linux__)
  // X11 / Wayland branches go here when extending to Linux.
#endif

WGPUSurface create_wgpu_surface_from_sdl(WGPUInstance instance, SDL_Window* window)
{
    if (!instance || !window) {
        return nullptr;
    }

    SDL_PropertiesID props = SDL_GetWindowProperties(window);

#if defined(_WIN32)
    HWND      hwnd      = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER,     nullptr);
    HINSTANCE hinstance = (HINSTANCE)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
    if (!hwnd) {
        return nullptr;
    }

    WGPUSurfaceDescriptorFromWindowsHWND chained{};
    chained.chain.sType = WGPUSType_SurfaceDescriptorFromWindowsHWND;
    chained.hinstance   = hinstance;
    chained.hwnd        = hwnd;

    WGPUSurfaceDescriptor desc{};
    desc.nextInChain = &chained.chain;

    return wgpuInstanceCreateSurface(instance, &desc);
#else
    (void)props;
    return nullptr;
#endif
}
