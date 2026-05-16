// Minimal SDL3 + wgpu-native + Dear ImGui host for HibitEcs.
//
// Native-first (Windows). The CMake target is gated by -DECS_BUILD_APP=ON
// and intentionally lives outside the ecs / ecs_bench test+benchmark builds.

#include <SDL3/SDL.h>
#include <webgpu/webgpu.h>
#include <wgpu/wgpu.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_wgpu.h>

#include <cstdio>
#include <cstdlib>

#include "wgpu_surface_sdl.hpp"
#include "playcanvas_theme.hpp"
#include "editor_ui.hpp"
#include "viewport_renderer.hpp"

namespace {

struct GpuContext {
    WGPUInstance        instance      = nullptr;
    WGPUAdapter         adapter       = nullptr;
    WGPUDevice          device        = nullptr;
    WGPUQueue           queue         = nullptr;
    WGPUSurface         surface       = nullptr;
    WGPUTextureFormat   surface_format = WGPUTextureFormat_Undefined;
    uint32_t            width         = 1280;
    uint32_t            height        = 720;
};

void on_request_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, char const* msg, void* userdata)
{
    auto* ctx = static_cast<GpuContext*>(userdata);
    if (status == WGPURequestAdapterStatus_Success) {
        ctx->adapter = adapter;
    } else {
        std::fprintf(stderr, "wgpu: RequestAdapter failed: %s\n", msg ? msg : "<no message>");
    }
}

void on_request_device(WGPURequestDeviceStatus status, WGPUDevice device, char const* msg, void* userdata)
{
    auto* ctx = static_cast<GpuContext*>(userdata);
    if (status == WGPURequestDeviceStatus_Success) {
        ctx->device = device;
    } else {
        std::fprintf(stderr, "wgpu: RequestDevice failed: %s\n", msg ? msg : "<no message>");
    }
}

void on_device_uncaptured_error(WGPUErrorType type, char const* msg, void* /*userdata*/)
{
    std::fprintf(stderr, "wgpu: uncaptured error (type=%d): %s\n", int(type), msg ? msg : "<no message>");
}

void configure_surface(GpuContext& ctx)
{
    WGPUSurfaceConfiguration cfg{};
    cfg.device      = ctx.device;
    cfg.format      = ctx.surface_format;
    cfg.usage       = WGPUTextureUsage_RenderAttachment;
    cfg.viewFormatCount = 0;
    cfg.viewFormats     = nullptr;
    cfg.alphaMode   = WGPUCompositeAlphaMode_Auto;
    cfg.width       = ctx.width;
    cfg.height      = ctx.height;
    cfg.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(ctx.surface, &cfg);
}

bool init_gpu(GpuContext& ctx, SDL_Window* window)
{
    WGPUInstanceDescriptor inst_desc{};
    ctx.instance = wgpuCreateInstance(&inst_desc);
    if (!ctx.instance) {
        std::fprintf(stderr, "wgpu: CreateInstance returned NULL\n");
        return false;
    }

    ctx.surface = create_wgpu_surface_from_sdl(ctx.instance, window);
    if (!ctx.surface) {
        std::fprintf(stderr, "wgpu: failed to create surface for SDL window\n");
        return false;
    }

    WGPURequestAdapterOptions opts{};
    opts.compatibleSurface = ctx.surface;
    opts.powerPreference   = WGPUPowerPreference_HighPerformance;
    wgpuInstanceRequestAdapter(ctx.instance, &opts, on_request_adapter, &ctx);
    if (!ctx.adapter) {
        return false;
    }

    WGPUDeviceDescriptor dev_desc{};
    dev_desc.label = "ecs_app device";
    dev_desc.uncapturedErrorCallbackInfo.callback = on_device_uncaptured_error;
    dev_desc.uncapturedErrorCallbackInfo.userdata = nullptr;
    wgpuAdapterRequestDevice(ctx.adapter, &dev_desc, on_request_device, &ctx);
    if (!ctx.device) {
        return false;
    }

    ctx.queue = wgpuDeviceGetQueue(ctx.device);

    WGPUSurfaceCapabilities caps{};
    wgpuSurfaceGetCapabilities(ctx.surface, ctx.adapter, &caps);
    ctx.surface_format = (caps.formatCount > 0) ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
    wgpuSurfaceCapabilitiesFreeMembers(caps);

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w > 0 && h > 0) {
        ctx.width  = static_cast<uint32_t>(w);
        ctx.height = static_cast<uint32_t>(h);
    }
    configure_surface(ctx);
    return true;
}

void shutdown_gpu(GpuContext& ctx)
{
    if (ctx.queue)    { wgpuQueueRelease(ctx.queue);    ctx.queue    = nullptr; }
    if (ctx.surface)  { wgpuSurfaceRelease(ctx.surface); ctx.surface = nullptr; }
    if (ctx.device)   { wgpuDeviceRelease(ctx.device);   ctx.device  = nullptr; }
    if (ctx.adapter)  { wgpuAdapterRelease(ctx.adapter); ctx.adapter = nullptr; }
    if (ctx.instance) { wgpuInstanceRelease(ctx.instance); ctx.instance = nullptr; }
}

} // namespace

int main(int /*argc*/, char** /*argv*/)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    SDL_Window* window = SDL_CreateWindow(
        "HibitEcs · ImGui + wgpu",
        1280, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    // SDL3 disables text input by default. ImGui's SDL3 backend only flips
    // it on for widgets that call ImGui's IME hook (InputText et al). The
    // script panel uses ImGuiColorTextEdit, which paints its own text and
    // pulls chars from io.InputQueueCharacters -- never triggering the IME
    // path. Enable text input globally so SDL_EVENT_TEXT_INPUT events flow.
    SDL_StartTextInput(window);

    GpuContext gpu;
    if (!init_gpu(gpu, window)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Body font: Roboto-Medium at ~14 px is the closest match for the
    // PlayCanvas editor's Source Sans-ish UI. The TTF is copied next to
    // ecs_app.exe by the CMake POST_BUILD step. Fall back to ImGui's
    // built-in ProggyClean if the file is missing.
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.PixelSnapH  = false;
        ImFont* roboto = io.Fonts->AddFontFromFileTTF("Roboto-Medium.ttf", 14.0f, &cfg);
        if (!roboto) {
            io.Fonts->AddFontDefault();
        }
    }

    ApplyPlayCanvasStyle();

    ImGui_ImplSDL3_InitForOther(window);

    ImGui_ImplWGPU_InitInfo wgpu_init{};
    wgpu_init.Device              = gpu.device;
    wgpu_init.NumFramesInFlight   = 3;
    wgpu_init.RenderTargetFormat  = gpu.surface_format;
    wgpu_init.DepthStencilFormat  = WGPUTextureFormat_Undefined;
    ImGui_ImplWGPU_Init(&wgpu_init);

    ViewportRenderer* viewport = viewport_renderer_create(gpu.device, gpu.queue);
    EditorSetViewportRenderer(viewport);

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                       ev.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            } else if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                       ev.type == SDL_EVENT_WINDOW_RESIZED) {
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(window, &w, &h);
                if (w > 0 && h > 0) {
                    gpu.width  = static_cast<uint32_t>(w);
                    gpu.height = static_cast<uint32_t>(h);
                    configure_surface(gpu);
                    ImGui_ImplWGPU_InvalidateDeviceObjects();
                    ImGui_ImplWGPU_CreateDeviceObjects();
                }
            }
        }

        ImGui_ImplWGPU_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        DrawPlayCanvasEditor();

        ImGui::Render();

        WGPUSurfaceTexture surface_tex{};
        wgpuSurfaceGetCurrentTexture(gpu.surface, &surface_tex);
        if (surface_tex.status != WGPUSurfaceGetCurrentTextureStatus_Success) {
            if (surface_tex.texture) {
                wgpuTextureRelease(surface_tex.texture);
            }
            continue;
        }

        WGPUTextureViewDescriptor view_desc{};
        view_desc.format          = wgpuTextureGetFormat(surface_tex.texture);
        view_desc.dimension       = WGPUTextureViewDimension_2D;
        view_desc.mipLevelCount   = 1;
        view_desc.arrayLayerCount = 1;
        view_desc.aspect          = WGPUTextureAspect_All;
        WGPUTextureView view = wgpuTextureCreateView(surface_tex.texture, &view_desc);

        WGPUCommandEncoderDescriptor enc_desc{};
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(gpu.device, &enc_desc);

        WGPURenderPassColorAttachment color_attach{};
        color_attach.view        = view;
        color_attach.loadOp      = WGPULoadOp_Clear;
        color_attach.storeOp     = WGPUStoreOp_Store;
        color_attach.clearValue  = WGPUColor{ 0.10, 0.10, 0.12, 1.0 };
        color_attach.depthSlice  = WGPU_DEPTH_SLICE_UNDEFINED;

        WGPURenderPassDescriptor pass_desc{};
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments     = &color_attach;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);

        ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);

        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);

        WGPUCommandBufferDescriptor cb_desc{};
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(encoder, &cb_desc);
        wgpuQueueSubmit(gpu.queue, 1, &cb);

        wgpuCommandBufferRelease(cb);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(view);
        wgpuSurfacePresent(gpu.surface);
        wgpuTextureRelease(surface_tex.texture);
    }

    EditorSetViewportRenderer(nullptr);
    viewport_renderer_destroy(viewport);

    ImGui_ImplWGPU_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    shutdown_gpu(gpu);

    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_SUCCESS;
}
