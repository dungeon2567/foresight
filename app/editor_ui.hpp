#pragma once

struct ViewportRenderer;

// Renders the PlayCanvas-style editor UI (menubar + toolbar + hierarchy +
// viewport + inspector + assets browser + status bar) into the current ImGui
// frame. Call once per frame, between ImGui::NewFrame() and ImGui::Render().
void DrawPlayCanvasEditor();

// Inject the (optional) 3D viewport renderer. When set, the Viewport panel
// renders a real wgpu scene into an offscreen texture and samples it via
// ImGui::Image; when null, it falls back to the 2D draw-list preview.
void EditorSetViewportRenderer(ViewportRenderer* r);
