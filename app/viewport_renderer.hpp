#pragma once

#include <webgpu/webgpu.h>
#include <imgui.h>
#include <cstdint>

// Offscreen 3D renderer for the editor viewport. Owns a render-target color
// texture (sampled by ImGui via ImTextureID) plus a matching depth buffer,
// a tiny render pipeline, cube geometry, and an MVP uniform buffer. Recreates
// the render targets on resize and the camera state from the supplied
// orbit/pan/zoom parameters every frame.
struct ViewportRenderer;

ViewportRenderer* viewport_renderer_create(WGPUDevice device, WGPUQueue queue);
void              viewport_renderer_destroy(ViewportRenderer* r);

// Render one frame at `pixel_w x pixel_h` and return a sampler-backed
// ImTextureID that the caller passes straight to ImGui::Image. Returns 0 if
// the size is degenerate or any GPU resource creation has failed.
ImTextureID viewport_renderer_render(ViewportRenderer* r,
                                     uint32_t pixel_w, uint32_t pixel_h,
                                     float yaw_deg, float pitch_deg,
                                     float zoom,
                                     float pan_x, float pan_y);
