#include "editor_ui.hpp"
#include "viewport_renderer.hpp"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API
#include <TextEditor.h>      // ImGuiColorTextEdit: highlight + auto-indent

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifndef SCRIPTS_DIR
#define SCRIPTS_DIR "scripts"
#endif

static ViewportRenderer* g_viewport_renderer = nullptr;

void EditorSetViewportRenderer(ViewportRenderer* r) { g_viewport_renderer = r; }

namespace {

// ---------------------------------------------------------------------------
// PlayCanvas palette (kept in sync with playcanvas_theme.hpp).
// ---------------------------------------------------------------------------
constexpr ImU32 PC_ORANGE      = IM_COL32(255, 102,   0, 255);
constexpr ImU32 PC_ORANGE_DIM  = IM_COL32(178,  71,   0, 255);
constexpr ImU32 PC_BG_DEEP     = IM_COL32( 30,  35,  38, 255);
constexpr ImU32 PC_PANEL       = IM_COL32( 46,  54,  58, 255);
constexpr ImU32 PC_PANEL_HL    = IM_COL32( 56,  65,  69, 255);
constexpr ImU32 PC_TEXT        = IM_COL32(234, 234, 234, 255);
constexpr ImU32 PC_TEXT_DIM    = IM_COL32(141, 148, 151, 255);

// ---------------------------------------------------------------------------
// Mock scene model.
// ---------------------------------------------------------------------------
struct Entity {
    std::string      name;
    std::string      tag;
    bool             enabled  = true;
    bool             expanded = true;
    float            pos[3]   = { 0.0f, 0.0f, 0.0f };
    float            rot[3]   = { 0.0f, 0.0f, 0.0f };
    float            scale[3] = { 1.0f, 1.0f, 1.0f };
    int              parent   = -1;
    std::vector<int> children;

    bool has_camera    = false;
    bool has_light     = false;
    bool has_render    = false;
    bool has_script    = false;
    bool has_rigidbody = false;
    bool has_collision = false;
};

enum AssetKind {
    ASSET_FOLDER,
    ASSET_MATERIAL,
    ASSET_TEXTURE,
    ASSET_SCRIPT,
    ASSET_MODEL,
    ASSET_AUDIO,
    ASSET_SCENE
};

struct Asset {
    std::string name;
    AssetKind   kind;
};

static std::vector<Entity> g_entities;
static int                 g_selected = -1;
static int                 g_root     = -1;

static std::vector<Asset>  g_assets;
static int                 g_asset_selected = -1;

// ---------------------------------------------------------------------------
// Layout constants — every panel pulls sizing from here. Tweak in one place,
// every header / row stays in sync.
// ---------------------------------------------------------------------------
constexpr float  kStatusBarHeight     = 26.0f;
constexpr ImVec2 kStatusWindowPadding = ImVec2(10, 4);
constexpr float  kInspectorLabelW     = 112.0f;     // x offset for value column
constexpr float  kAssetTileSize       = 92.0f;
constexpr float  kAssetTilePad        = 6.0f;
constexpr float  kIconButtonSize      = 24.0f;

// Editor mode toggles.
static char g_search[64] = "";

// Faux viewport camera (mouse-driven). Drives the grid / box drawing so the
// user sees the scene respond when they orbit / pan / zoom.
static float g_cam_yaw   = 0.0f;
static float g_cam_pitch = 0.0f;
static float g_cam_pan_x = 0.0f;
static float g_cam_pan_y = 0.0f;
static float g_cam_zoom  = 1.0f;

// ---------------------------------------------------------------------------
// One-time scene + asset seed.
// ---------------------------------------------------------------------------
static int add_entity(const char* name, int parent)
{
    Entity e;
    e.name   = name;
    e.parent = parent;
    int idx  = (int)g_entities.size();
    g_entities.push_back(e);
    if (parent >= 0) {
        g_entities[parent].children.push_back(idx);
    }
    return idx;
}

static void seed_scene_once()
{
    if (!g_entities.empty()) {
        return;
    }

    int root = add_entity("Root", -1);
    g_root = root;

    int camera = add_entity("Camera", root);
    g_entities[camera].has_camera = true;
    g_entities[camera].pos[1] = 1.8f;
    g_entities[camera].pos[2] = 5.5f;
    g_entities[camera].rot[0] = -10.0f;

    int sun = add_entity("Directional Light", root);
    g_entities[sun].has_light = true;
    g_entities[sun].rot[0] = 45.0f;
    g_entities[sun].rot[1] = 30.0f;

    int env = add_entity("Environment", root);
    add_entity("Skybox",   env);
    int ground = add_entity("Ground",   env);
    g_entities[ground].has_render    = true;
    g_entities[ground].has_collision = true;
    g_entities[ground].scale[0] = 20.0f;
    g_entities[ground].scale[2] = 20.0f;

    int player = add_entity("Player", root);
    g_entities[player].tag             = "player";
    g_entities[player].has_render      = true;
    g_entities[player].has_rigidbody   = true;
    g_entities[player].has_collision   = true;
    g_entities[player].has_script      = true;
    int weapons = add_entity("Weapons", player);
    add_entity("PrimaryGun", weapons);
    add_entity("Sidearm",    weapons);

    int enemies = add_entity("Enemies", root);
    add_entity("Enemy_01", enemies);
    add_entity("Enemy_02", enemies);
    add_entity("Enemy_03", enemies);

    int ui = add_entity("UI", root);
    add_entity("HUD",       ui);
    add_entity("PauseMenu", ui);

    g_selected = player;

    g_assets = {
        { "materials",   ASSET_FOLDER   },
        { "textures",    ASSET_FOLDER   },
        { "scripts",     ASSET_FOLDER   },
        { "models",      ASSET_FOLDER   },
        { "audio",       ASSET_FOLDER   },
        { "scenes",      ASSET_FOLDER   },
        { "Main.scene",  ASSET_SCENE    },
        { "player.mat",  ASSET_MATERIAL },
        { "ground.mat",  ASSET_MATERIAL },
        { "sky.mat",     ASSET_MATERIAL },
        { "albedo.png",  ASSET_TEXTURE  },
        { "normal.png",  ASSET_TEXTURE  },
        { "rough.png",   ASSET_TEXTURE  },
        { "player.js",   ASSET_SCRIPT   },
        { "enemy.js",    ASSET_SCRIPT   },
        { "weapon.js",   ASSET_SCRIPT   },
        { "player.glb",  ASSET_MODEL    },
        { "enemy.glb",   ASSET_MODEL    },
        { "map.glb",     ASSET_MODEL    },
        { "shoot.ogg",   ASSET_AUDIO    },
        { "music.ogg",   ASSET_AUDIO    },
    };
}

// ---------------------------------------------------------------------------
// Small UI helpers.
// ---------------------------------------------------------------------------
static const char* asset_kind_label(AssetKind k)
{
    switch (k) {
        case ASSET_FOLDER:   return "Folder";
        case ASSET_MATERIAL: return "Material";
        case ASSET_TEXTURE:  return "Texture";
        case ASSET_SCRIPT:   return "Script";
        case ASSET_MODEL:    return "Model";
        case ASSET_AUDIO:    return "Audio";
        case ASSET_SCENE:    return "Scene";
    }
    return "?";
}

static ImU32 asset_kind_color(AssetKind k)
{
    switch (k) {
        case ASSET_FOLDER:   return IM_COL32(255, 153,  51, 255);
        case ASSET_MATERIAL: return IM_COL32( 89, 140, 200, 255);
        case ASSET_TEXTURE:  return IM_COL32(196, 196, 196, 255);
        case ASSET_SCRIPT:   return IM_COL32(245, 210,  80, 255);
        case ASSET_MODEL:    return IM_COL32(120, 200, 140, 255);
        case ASSET_AUDIO:    return IM_COL32(200, 120, 200, 255);
        case ASSET_SCENE:    return IM_COL32(255, 102,   0, 255);
    }
    return IM_COL32(180, 180, 180, 255);
}

static const char* entity_glyph(const Entity& e)
{
    if (e.has_camera)    return "[C]";
    if (e.has_light)     return "[L]";
    if (e.has_render)    return "[R]";
    if (e.has_script)    return "[S]";
    if (!e.children.empty()) return "[G]";
    return "[E]";
}

static void draw_tag_chip(const char* text)
{
    // Match the height of a regular ImGui::Button so chips align with the
    // "+ Add" button on the same row.
    ImVec2 sz   = ImGui::CalcTextSize(text);
    ImVec2 pad  = ImGui::GetStyle().FramePadding;
    ImVec2 p    = ImGui::GetCursorScreenPos();
    ImVec2 size = ImVec2(sz.x + pad.x * 2.0f, sz.y + pad.y * 2.0f);
    ImGui::Dummy(size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), PC_PANEL_HL, 2.0f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), PC_TEXT, text);
}

// Manually-centered glyph button. Roboto's "+" sits ~1 px above the metric
// center, so we draw the glyph ourselves to nudge it back. Used for tiny
// header buttons in the hierarchy / assets toolbars where visual centering
// matters more than the default Button widget.
static bool IconButton(const char* id, const char* glyph, float size = kIconButtonSize,
                       bool accent = false)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    ImU32 bg;
    if (accent) {
        bg = active ? PC_ORANGE_DIM
                    : (hovered ? IM_COL32(255, 125, 40, 255) : PC_ORANGE);
    } else {
        bg = active ? PC_ORANGE
                    : (hovered ? PC_PANEL_HL : PC_PANEL);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), bg, 2.0f);

    ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(
        ImVec2(p.x + (size - ts.x) * 0.5f,
               p.y + (size - ts.y) * 0.5f - 1.0f),  // baseline nudge
        PC_TEXT, glyph);
    return pressed;
}

// Frame-padded label for inspector rows — keeps the text vertically aligned
// with the widget that follows on `SameLine`. Always pair with SameLine().
static void InspectorLabel(const char* label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kInspectorLabelW);
}

static bool AccentButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    ImGui::PushStyleColor(ImGuiCol_Button,        PC_ORANGE);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 125, 40, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(217,  86,  0, 255));
    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(255, 255, 255, 255));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// ---------------------------------------------------------------------------
// Hierarchy panel.
// ---------------------------------------------------------------------------
static void DrawHierarchyNode(int idx, int depth)
{
    Entity& e = g_entities[idx];

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_OpenOnDoubleClick |
        ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_FramePadding;
    if (e.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (g_selected == idx) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (e.expanded) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }

    ImGui::PushStyleColor(ImGuiCol_Text,
        e.enabled ? PC_TEXT : PC_TEXT_DIM);

    char label[256];
    std::snprintf(label, sizeof(label), "%s  %s", entity_glyph(e), e.name.c_str());
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)idx, flags, "%s", label);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        g_selected = idx;
    }

    ImGui::PopStyleColor();

    if (open && !e.children.empty()) {
        for (int c : e.children) {
            DrawHierarchyNode(c, depth + 1);
        }
        ImGui::TreePop();
    }
}

static void DrawHierarchy()
{
    // Search box
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##hier_search", "Search entities...", g_search, IM_ARRAYSIZE(g_search));

    ImGui::Separator();

    // Tree
    ImGui::BeginChild("##hier_tree", ImVec2(0, 0), ImGuiChildFlags_None);
    if (g_root >= 0) {
        DrawHierarchyNode(g_root, 0);
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Viewport panel — wgpu-rendered 3D scene drawn into an offscreen texture
// and sampled back via ImGui::Image. Mouse drag / wheel feed an orbit camera.
// ---------------------------------------------------------------------------
static void DrawViewport()
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f || avail.y < 1.0f) {
        return;
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + avail.x, p0.y + avail.y);

    // Claim the panel as an interactive surface BEFORE drawing so it captures
    // clicks/drag/wheel instead of letting them fall through to the dock host.
    ImGui::InvisibleButton("##vp_input", avail,
        ImGuiButtonFlags_MouseButtonLeft  |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    const bool vp_hovered = ImGui::IsItemHovered();
    const bool vp_active  = ImGui::IsItemActive();
    const ImVec2 md       = ImGui::GetIO().MouseDelta;

    if (vp_active) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            g_cam_yaw   += md.x * 0.4f;
            g_cam_pitch += md.y * 0.4f;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            g_cam_pan_x += md.x;
            g_cam_pan_y += md.y;
        }
    }
    if (vp_hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            g_cam_zoom *= (wheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
            if (g_cam_zoom < 0.1f)  g_cam_zoom = 0.1f;
            if (g_cam_zoom > 10.0f) g_cam_zoom = 10.0f;
        }
    }

    ImGui::SetCursorScreenPos(p0);

    ImTextureID tex = 0;
    if (g_viewport_renderer) {
        const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
        const uint32_t pw = (uint32_t)(avail.x * (fb_scale.x > 0.0f ? fb_scale.x : 1.0f));
        const uint32_t ph = (uint32_t)(avail.y * (fb_scale.y > 0.0f ? fb_scale.y : 1.0f));
        tex = viewport_renderer_render(g_viewport_renderer, pw, ph,
            g_cam_yaw, g_cam_pitch, g_cam_zoom, g_cam_pan_x, g_cam_pan_y);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex) {
        dl->AddImage(tex, p0, p1);
    } else {
        dl->AddRectFilled(p0, p1, IM_COL32(20, 24, 28, 255));
        const char* msg = "viewport renderer not available";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (avail.x - ts.x) * 0.5f,
                           p0.y + (avail.y - ts.y) * 0.5f),
                    PC_TEXT_DIM, msg);
    }

    // Axis gizmo overlay (drawn on top of the rendered image).
    {
        ImVec2 g(p1.x - 48.0f, p0.y + 48.0f);
        const float yaw_rad = g_cam_yaw * 0.0174533f;
        const float c = std::cos(yaw_rad), s = std::sin(yaw_rad);
        ImVec2 ex(g.x + 18.0f * c,    g.y - 18.0f * s * 0.4f);
        ImVec2 ez(g.x + 18.0f * (-s), g.y - 18.0f * c * 0.4f);
        dl->AddCircleFilled(g, 24.0f, IM_COL32(0, 0, 0, 120), 24);
        dl->AddLine(g, ex,                                IM_COL32(232,  80,  80, 255), 2.0f);
        dl->AddLine(g, ImVec2(g.x, g.y - 18.0f),          IM_COL32(120, 200, 120, 255), 2.0f);
        dl->AddLine(g, ez,                                IM_COL32( 80, 140, 232, 255), 2.0f);
        dl->AddText(ImVec2(ex.x + 2.0f, ex.y - 6.0f),     IM_COL32(232,  80,  80, 255), "X");
        dl->AddText(ImVec2(g.x - 4.0f,  g.y - 30.0f),     IM_COL32(120, 200, 120, 255), "Y");
        dl->AddText(ImVec2(ez.x - 8.0f, ez.y + 2.0f),     IM_COL32( 80, 140, 232, 255), "Z");
    }

    // Bottom-left overlay: camera state.
    char overlay[96];
    std::snprintf(overlay, sizeof(overlay),
        "yaw %.0f  pitch %.0f  zoom %.2fx  pan (%.0f,%.0f)",
        g_cam_yaw, g_cam_pitch, g_cam_zoom, g_cam_pan_x, g_cam_pan_y);
    dl->AddText(ImVec2(p0.x + 8.0f, p1.y - 20.0f), PC_TEXT_DIM, overlay);
}

// ---------------------------------------------------------------------------
// Inspector — properties of the selected entity.
// ---------------------------------------------------------------------------
static bool ComponentHeader(const char* label, bool* keep_open)
{
    ImGui::PushStyleColor(ImGuiCol_Header,        PC_PANEL);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, PC_PANEL_HL);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  PC_PANEL_HL);
    bool open = ImGui::CollapsingHeader(label,
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopStyleColor(3);

    // Right-aligned "x" delete glyph drawn directly on the header rect so it
    // visually centers regardless of font baseline quirks.
    ImVec2 hmin = ImGui::GetItemRectMin();
    ImVec2 hmax = ImGui::GetItemRectMax();
    float  hh   = hmax.y - hmin.y;
    ImVec2 btn_min(hmax.x - hh, hmin.y);
    ImVec2 btn_max(hmax.x,      hmax.y);

    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(btn_min);
    bool del_pressed = ImGui::InvisibleButton("##del", ImVec2(hh, hh));
    bool del_hover   = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (del_hover) {
        dl->AddRectFilled(btn_min, btn_max, IM_COL32(255, 80, 80, 60), 2.0f);
    }
    const char* g = "x";
    ImVec2 ts = ImGui::CalcTextSize(g);
    dl->AddText(
        ImVec2(btn_min.x + (hh - ts.x) * 0.5f,
               btn_min.y + (hh - ts.y) * 0.5f - 1.0f),
        del_hover ? IM_COL32(255, 200, 200, 255) : PC_TEXT_DIM, g);
    ImGui::PopID();
    if (del_pressed) {
        *keep_open = false;
    }
    return open;
}

static void DragVec3(const char* id, float v[3])
{
    ImGui::PushID(id);
    float w = (ImGui::GetContentRegionAvail().x - 12.0f) / 3.0f;
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(80, 28, 28, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(110, 40, 40, 255));
    ImGui::DragFloat("##x", &v[0], 0.05f, 0.0f, 0.0f, "X %.2f");
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(28, 70, 28, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(40, 96, 40, 255));
    ImGui::DragFloat("##y", &v[1], 0.05f, 0.0f, 0.0f, "Y %.2f");
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(28, 44, 90, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(40, 64, 120, 255));
    ImGui::DragFloat("##z", &v[2], 0.05f, 0.0f, 0.0f, "Z %.2f");
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

static void DrawInspector()
{
    if (g_selected < 0) {
        ImGui::TextDisabled("Select an entity to inspect.");
        return;
    }
    Entity& e = g_entities[g_selected];

    // Inspector-wide tighter spacing for a denser PlayCanvas-style layout.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(6, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6, 4));

    // Entity header: enabled toggle + name field.
    ImGui::Checkbox("##enabled", &e.enabled);
    ImGui::SameLine();
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s", e.name.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##name", name_buf, sizeof(name_buf))) {
        e.name = name_buf;
    }

    ImGui::TextDisabled("entity #%d  ·  guid 0x%08X", g_selected, 0xA5C0FFEE + g_selected);

    // Tags row — frame-pad the "Tags" label so it baselines with the chips
    // and the "+ Add" button.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Tags");
    ImGui::SameLine(kInspectorLabelW);
    if (!e.tag.empty()) {
        draw_tag_chip(e.tag.c_str());
        ImGui::SameLine(0.0f, 4.0f);
    }
    ImGui::Button("+ Add", ImVec2(56, 0));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Transform
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        InspectorLabel("Position"); DragVec3("pos",   e.pos);
        InspectorLabel("Rotation"); DragVec3("rot",   e.rot);
        InspectorLabel("Scale");    DragVec3("scale", e.scale);
    }

    // Components.
    if (e.has_camera) {
        if (ComponentHeader("Camera", &e.has_camera)) {
            static int proj = 0;
            const char* projs[] = { "Perspective", "Orthographic" };
            InspectorLabel("Projection");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::Combo("##proj", &proj, projs, IM_ARRAYSIZE(projs));
            static float fov = 60.0f, np = 0.1f, fp = 1000.0f;
            InspectorLabel("FOV");        ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##fov", &fov, 0.5f, 10.0f, 170.0f, "%.1f");
            InspectorLabel("Near Clip");  ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##np",  &np,  0.01f, 0.001f, 100.0f, "%.3f");
            InspectorLabel("Far Clip");   ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##fp",  &fp,  1.0f, 1.0f, 10000.0f, "%.1f");
            static float clear[4] = { 0.118f, 0.137f, 0.149f, 1.0f };
            InspectorLabel("Clear Color"); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::ColorEdit4("##clear", clear);
            static int prio = 0;
            InspectorLabel("Priority");    ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragInt("##prio", &prio, 1, 0, 32);
        }
    }
    if (e.has_light) {
        if (ComponentHeader("Light", &e.has_light)) {
            static int type = 0;
            const char* types[] = { "Directional", "Point", "Spot" };
            InspectorLabel("Type");      ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Combo("##ltype", &type, types, IM_ARRAYSIZE(types));
            static float color[3] = { 1.0f, 0.95f, 0.85f };
            InspectorLabel("Color");     ImGui::SetNextItemWidth(-FLT_MIN); ImGui::ColorEdit3("##lcol", color);
            static float intensity = 1.0f;
            InspectorLabel("Intensity"); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##lint", &intensity, 0.05f, 0.0f, 32.0f, "%.2f");
            static float range = 12.0f;
            InspectorLabel("Range");     ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##lrange", &range, 0.1f, 0.0f, 1000.0f, "%.2f");
            static bool shadows = true;
            InspectorLabel("Cast Shadows"); ImGui::Checkbox("##lshadows", &shadows);
        }
    }
    if (e.has_render) {
        if (ComponentHeader("Render", &e.has_render)) {
            static int mtype = 0;
            const char* meshes[] = { "Asset", "Box", "Sphere", "Cylinder", "Plane" };
            InspectorLabel("Type");  ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Combo("##mtype", &mtype, meshes, IM_ARRAYSIZE(meshes));
            InspectorLabel("Asset"); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Button("player.glb", ImVec2(-FLT_MIN, 0));
            static bool cast = true, recv = true;
            InspectorLabel("Cast Shadows");    ImGui::Checkbox("##cs", &cast);
            InspectorLabel("Receive Shadows"); ImGui::Checkbox("##rs", &recv);
        }
    }
    if (e.has_rigidbody) {
        if (ComponentHeader("Rigid Body", &e.has_rigidbody)) {
            static int rtype = 1;
            const char* rtypes[] = { "Static", "Dynamic", "Kinematic" };
            InspectorLabel("Type"); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Combo("##rtype", &rtype, rtypes, IM_ARRAYSIZE(rtypes));
            static float mass = 1.0f, fric = 0.5f, rest = 0.0f;
            InspectorLabel("Mass");        ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##mass", &mass, 0.01f, 0.0f, 1000.0f, "%.2f");
            InspectorLabel("Friction");    ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##fric", &fric, 0.01f, 0.0f, 1.0f, "%.2f");
            InspectorLabel("Restitution"); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##rest", &rest, 0.01f, 0.0f, 1.0f, "%.2f");
        }
    }
    if (e.has_collision) {
        if (ComponentHeader("Collision", &e.has_collision)) {
            static int ctype = 0;
            const char* ctypes[] = { "Box", "Sphere", "Capsule", "Cylinder", "Mesh" };
            InspectorLabel("Type"); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Combo("##ctype", &ctype, ctypes, IM_ARRAYSIZE(ctypes));
            static float half[3] = { 0.5f, 0.5f, 0.5f };
            InspectorLabel("Half Extents"); DragVec3("half", half);
        }
    }
    if (e.has_script) {
        if (ComponentHeader("Script", &e.has_script)) {
            ImGui::TextDisabled("Scripts attached:");
            ImGui::BulletText("player.js");
            ImGui::BulletText("weapon.js");
            ImGui::Spacing();
            if (AccentButton("+ Add Script", ImVec2(-FLT_MIN, 0))) {}
        }
    }

    ImGui::Spacing();
    if (AccentButton("+ ADD COMPONENT", ImVec2(-FLT_MIN, 28.0f))) {
        ImGui::OpenPopup("##add_comp");
    }
    if (ImGui::BeginPopup("##add_comp")) {
        if (ImGui::MenuItem("Camera"))     e.has_camera    = true;
        if (ImGui::MenuItem("Light"))      e.has_light     = true;
        if (ImGui::MenuItem("Render"))     e.has_render    = true;
        if (ImGui::MenuItem("Rigid Body")) e.has_rigidbody = true;
        if (ImGui::MenuItem("Collision"))  e.has_collision = true;
        if (ImGui::MenuItem("Script"))     e.has_script    = true;
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------
// Script editor — replaces the Inspector panel. Lists *.script files from
// SCRIPTS_DIR, loads the selected one into a ImGuiColorTextEdit TextEditor
// (syntax highlight + auto-indent + line numbers), writes it back on Save.
// ---------------------------------------------------------------------------
struct ScriptFile {
    std::string path;
    std::string name;
};

static std::vector<ScriptFile> g_script_files;
static int                     g_script_selected = -1;
static TextEditor              g_script_editor;
static bool                    g_script_editor_inited = false;
static bool                    g_script_dirty   = false;
static std::string             g_script_status;     // last load/save message
static bool                    g_script_seeded  = false;

static TextEditor::LanguageDefinition build_script_language()
{
    // Keyword set covers prefab / ability / effect / command / input
    // structural keywords plus the expression-level vocabulary used inside
    // `on`/`every` blocks and requirement queries.
    TextEditor::LanguageDefinition lang;
    static const char* const kws[] = {
        "command","prefab","ability","effect","input",
        "tags","attributes","requirements","costs","cooldowns",
        "owned_tags","duration","period","every","on",
        "if","else","all","any","none",
        "emit","apply","despawn",
        "self","event",
        "entity","point","button","stick","fixed",
    };
    for (const char* k : kws) {
        lang.mKeywords.insert(k);
    }

    lang.mCommentStart       = "/*";
    lang.mCommentEnd         = "*/";
    lang.mSingleLineComment  = "//";
    lang.mCaseSensitive      = true;
    lang.mAutoIndentation    = true;
    lang.mName               = "script";

    // Single-line comment / identifier / number / punctuation regex tokens.
    // Identifier regex accepts dotted names like `damage.fire`, matching the
    // hierarchical-tag form used by the runtime.
    lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
        "//.*",                                  TextEditor::PaletteIndex::Comment));
    lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
        "[a-zA-Z_][a-zA-Z0-9_.]*",               TextEditor::PaletteIndex::Identifier));
    lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
        "[+\\-]?[0-9]+(\\.[0-9]+)?[sS]?",        TextEditor::PaletteIndex::Number));
    lang.mTokenRegexStrings.push_back(std::make_pair<std::string, TextEditor::PaletteIndex>(
        "[\\{\\}\\(\\)\\[\\];,.<>=+\\-*/%&|^~!]",TextEditor::PaletteIndex::Punctuation));

    return lang;
}

static void scan_scripts_dir()
{
    g_script_files.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it(SCRIPTS_DIR, ec);
    if (ec) {
        g_script_status = std::string("scan failed: ") + SCRIPTS_DIR;
        return;
    }
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".script") continue;
        ScriptFile f;
        f.path = entry.path().string();
        f.name = entry.path().filename().string();
        g_script_files.push_back(std::move(f));
    }
    std::sort(g_script_files.begin(), g_script_files.end(),
        [](const ScriptFile& a, const ScriptFile& b) { return a.name < b.name; });
}

static void script_load(int idx)
{
    if (idx < 0 || idx >= (int)g_script_files.size()) {
        return;
    }
    std::ifstream f(g_script_files[idx].path, std::ios::binary);
    if (!f) {
        g_script_status = "load failed: " + g_script_files[idx].name;
        return;
    }
    std::string contents((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    g_script_editor.SetText(contents);
    // SetText flips the change flag; clear it so the buffer reads "clean".
    (void)g_script_editor.IsTextChanged();
    g_script_selected = idx;
    g_script_dirty    = false;
    g_script_status   = "loaded " + g_script_files[idx].name;
}

static void script_save()
{
    if (g_script_selected < 0) return;
    const auto& sf = g_script_files[g_script_selected];
    std::ofstream f(sf.path, std::ios::binary | std::ios::trunc);
    if (!f) {
        g_script_status = "save failed: " + sf.name;
        return;
    }
    const std::string text = g_script_editor.GetText();
    f.write(text.data(), (std::streamsize)text.size());
    g_script_dirty  = false;
    g_script_status = "saved " + sf.name;
}

// ---------------------------------------------------------------------------
// Autocomplete. ImGuiColorTextEdit has no built-in completion, so we layer
// our own popup. Trigger is Ctrl+Space. Pool = language keywords union all
// identifier tokens scraped from the live buffer (so symbols you typed in
// the same file complete too). Popup uses ImGui::BeginPopup, which grabs
// focus -- TextEditor's HandleKeyboardInputs gates on IsWindowFocused(), so
// arrows/Enter/Tab route to the popup while it's open.
// ---------------------------------------------------------------------------
struct AutocompleteState {
    std::vector<std::string> matches;
    int                      sel        = 0;
    std::string              prefix;
    ImVec2                   anchor_pos = ImVec2(0.0f, 0.0f);
};
static AutocompleteState g_ac;

static std::vector<std::string> ac_collect_pool()
{
    std::vector<std::string> pool;
    for (const auto& kw : g_script_editor.GetLanguageDefinition().mKeywords) {
        pool.push_back(kw);
    }
    auto is_id_start = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    auto is_id_part = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '.';
    };
    std::string text = g_script_editor.GetText();
    size_t i = 0;
    while (i < text.size()) {
        if (is_id_start(text[i])) {
            size_t j = i + 1;
            while (j < text.size() && is_id_part(text[j])) ++j;
            pool.push_back(text.substr(i, j - i));
            i = j;
        } else {
            ++i;
        }
    }
    std::sort(pool.begin(), pool.end());
    pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
    return pool;
}

static std::string ac_prefix_at_cursor()
{
    // GetText(start, end) and Backspace are private in BalazsJako, so we
    // reconstruct prefix from the public line API. mColumn is a *display*
    // column (tabs expand to tab_size - col%tab_size), so we walk the raw
    // line tracking display columns to find the cursor's byte index.
    auto cur = g_script_editor.GetCursorPosition();
    const std::string line = g_script_editor.GetCurrentLineText();
    const int tab_size = g_script_editor.GetTabSize() > 0
                       ? g_script_editor.GetTabSize() : 4;
    int col = 0;
    size_t idx = 0;
    while (idx < line.size() && col < cur.mColumn) {
        if (line[idx] == '\t') {
            col += tab_size - (col % tab_size);
        } else {
            ++col;
        }
        ++idx;
    }
    size_t j = idx;
    while (j > 0) {
        char c = line[j - 1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.') --j;
        else break;
    }
    return line.substr(j, idx - j);
}

static void ac_refresh_matches()
{
    auto pool = ac_collect_pool();
    g_ac.matches.clear();
    for (const auto& s : pool) {
        if (s.size() >= g_ac.prefix.size() &&
            std::strncmp(s.c_str(), g_ac.prefix.c_str(),
                         g_ac.prefix.size()) == 0 &&
            s != g_ac.prefix) {
            g_ac.matches.push_back(s);
        }
    }
    if (g_ac.sel >= (int)g_ac.matches.size()) g_ac.sel = 0;
    if (g_ac.sel < 0) g_ac.sel = 0;
}

static void ac_accept()
{
    if (g_ac.sel < 0 || g_ac.sel >= (int)g_ac.matches.size()) return;
    const std::string full = g_ac.matches[g_ac.sel];
    // Backspace is private; replace prefix via public SetSelection + Delete
    // + InsertText. Prefix is word chars only (1 display column each), so
    // its column span is exactly prefix.size().
    auto cur = g_script_editor.GetCursorPosition();
    TextEditor::Coordinates start(cur.mLine,
        cur.mColumn - (int)g_ac.prefix.size());
    g_script_editor.SetSelection(start, cur);
    g_script_editor.Delete();
    g_script_editor.InsertText(full);
    g_script_dirty = true;
}

static void DrawScriptEditor()
{
    if (!g_script_editor_inited) {
        g_script_editor_inited = true;
        g_script_editor.SetLanguageDefinition(build_script_language());
        g_script_editor.SetPalette(TextEditor::GetDarkPalette());
        g_script_editor.SetShowWhitespaces(false);
        g_script_editor.SetTabSize(4);
    }
    if (!g_script_seeded) {
        g_script_seeded = true;
        scan_scripts_dir();
        // Prefer player.script when present so the panel matches the user's
        // request out of the box.
        int prefer = -1;
        for (int i = 0; i < (int)g_script_files.size(); ++i) {
            if (g_script_files[i].name == "player.script") { prefer = i; break; }
        }
        if (prefer < 0 && !g_script_files.empty()) prefer = 0;
        if (prefer >= 0) script_load(prefer);
    }

    // Toolbar: file dropdown + Rescan + Reload + Save + status.
    const char* cur = (g_script_selected >= 0)
        ? g_script_files[g_script_selected].name.c_str()
        : "<no .script files>";
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##script_file", cur)) {
        for (int i = 0; i < (int)g_script_files.size(); ++i) {
            const bool selected = (i == g_script_selected);
            if (ImGui::Selectable(g_script_files[i].name.c_str(), selected)) {
                script_load(i);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        const std::string keep = (g_script_selected >= 0)
            ? g_script_files[g_script_selected].path : std::string();
        scan_scripts_dir();
        int found = -1;
        for (int i = 0; i < (int)g_script_files.size(); ++i) {
            if (g_script_files[i].path == keep) { found = i; break; }
        }
        g_script_selected = found;
        if (found < 0) {
            g_script_editor.SetText("");
            (void)g_script_editor.IsTextChanged();
            g_script_dirty = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        if (g_script_selected >= 0) script_load(g_script_selected);
    }
    ImGui::SameLine();
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        PC_ORANGE);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 125, 40, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  PC_ORANGE_DIM);
        ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(255, 255, 255, 255));
        const bool can_save = (g_script_selected >= 0);
        ImGui::BeginDisabled(!can_save);
        if (ImGui::Button("Save")) {
            script_save();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
    }
    ImGui::SameLine();
    if (g_script_dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "*modified");
    } else if (!g_script_status.empty()) {
        ImGui::TextDisabled("%s", g_script_status.c_str());
    }

    // Cursor / line info on the far right of the toolbar row.
    {
        auto coord = g_script_editor.GetCursorPosition();
        char info[64];
        std::snprintf(info, sizeof(info), "Ln %d  Col %d  |  %d lines",
            coord.mLine + 1, coord.mColumn + 1,
            g_script_editor.GetTotalLines());
        const float text_w = ImGui::CalcTextSize(info).x;
        const float avail_x = ImGui::GetContentRegionAvail().x;
        if (avail_x > text_w + 8.0f) {
            ImGui::SameLine(ImGui::GetCursorPosX() + (avail_x - text_w - 4.0f));
        }
        ImGui::TextDisabled("%s", info);
    }

    ImGui::Separator();

    // OR the editor's per-frame change flag into our sticky dirty flag.
    if (g_script_editor.IsTextChanged()) {
        g_script_dirty = true;
    }

    const bool panel_focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    // Ctrl+S → save. TextEditor doesn't consume this combo so the panel
    // can claim it when focused (including its child editor view).
    if (panel_focused && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        script_save();
    }

    // Ctrl+Space → open autocomplete. Computed BEFORE editor.Render so the
    // popup gets queued first; popup focus blocks TextEditor's key handling
    // on subsequent frames (its HandleKeyboardInputs is gated by
    // IsWindowFocused()).
    if (panel_focused && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false) &&
        !ImGui::IsPopupOpen("##ac_popup")) {
        g_ac.prefix = ac_prefix_at_cursor();
        ac_refresh_matches();
        if (!g_ac.matches.empty()) {
            // Approximate cursor screen position from panel origin +
            // editor metrics. Roboto isn't monospace so this drifts a few
            // pixels per column, good enough for placement.
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const auto   cc     = g_script_editor.GetCursorPosition();
            const float  line_h = ImGui::GetTextLineHeight();
            const float  char_w = ImGui::GetFont()->GetCharAdvance('M');
            g_ac.anchor_pos = ImVec2(
                origin.x + 56.0f + (float)cc.mColumn * char_w,
                origin.y + (float)(cc.mLine + 1) * line_h + 2.0f);
            g_ac.sel = 0;
            ImGui::OpenPopup("##ac_popup");
        }
    }

    // Render() consumes the remaining content region; pass an explicit size
    // so the editor fills the panel.
    g_script_editor.Render("##script_text", ImGui::GetContentRegionAvail(), false);

    // Autocomplete popup. BeginPopup grabs focus → TextEditor's
    // HandleKeyboardInputs sees IsWindowFocused() == false and skips its
    // arrow/Enter/Tab handlers, so those keys belong to the popup.
    if (ImGui::IsPopupOpen("##ac_popup")) {
        ImGui::SetNextWindowPos(g_ac.anchor_pos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("##ac_popup",
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_AlwaysAutoResize)) {

            const int n = (int)g_ac.matches.size();
            if (n > 0 && ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                g_ac.sel = (g_ac.sel + 1) % n;
            }
            if (n > 0 && ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                g_ac.sel = (g_ac.sel - 1 + n) % n;
            }
            bool accept = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_Tab,   false);

            for (int i = 0; i < n; ++i) {
                const bool sel = (i == g_ac.sel);
                if (ImGui::Selectable(g_ac.matches[i].c_str(), sel)) {
                    g_ac.sel = i;
                    accept = true;
                }
                if (sel && ImGui::IsWindowAppearing()) {
                    ImGui::SetScrollHereY();
                }
            }

            if (accept) {
                ac_accept();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

// ---------------------------------------------------------------------------
// Asset browser at the bottom.
// ---------------------------------------------------------------------------
static void DrawAssets()
{
    // Breadcrumb + filter row (the dock tab already supplies the panel title).
    ImGui::TextDisabled("/ root /");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 252.0f);
    static int filter = 0;
    ImGui::SetNextItemWidth(120.0f);
    const char* filters[] = { "All", "Material", "Texture", "Script", "Model", "Audio", "Scene" };
    ImGui::Combo("##afilter", &filter, filters, IM_ARRAYSIZE(filters));
    ImGui::SameLine();
    static char asearch[64] = "";
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputTextWithHint("##asearch", "Search...", asearch, IM_ARRAYSIZE(asearch));

    ImGui::Separator();

    ImGui::BeginChild("##assets_body", ImVec2(0, 0), ImGuiChildFlags_None);

    const float tile_w = kAssetTileSize;
    const float tile_h = kAssetTileSize;
    const float pad    = kAssetTilePad;
    float avail_w = ImGui::GetContentRegionAvail().x;
    int cols = (int)((avail_w + pad) / (tile_w + pad));
    if (cols < 1) cols = 1;

    for (int i = 0; i < (int)g_assets.size(); ++i) {
        const Asset& a = g_assets[i];
        if (i % cols != 0) ImGui::SameLine(0.0f, pad);

        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tile", ImVec2(tile_w, tile_h))) {
            g_asset_selected = i;
        }
        bool hovered  = ImGui::IsItemHovered();
        bool selected = (g_asset_selected == i);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p1(p.x + tile_w, p.y + tile_h);
        dl->AddRectFilled(p, p1, selected ? PC_PANEL_HL : PC_BG_DEEP, 2.0f);
        if (selected || hovered) {
            dl->AddRect(p, p1, selected ? PC_ORANGE : PC_PANEL_HL, 2.0f, 0, 1.5f);
        }

        // Thumbnail
        ImVec2 t0(p.x + 10.0f, p.y + 8.0f);
        ImVec2 t1(p.x + tile_w - 10.0f, p.y + tile_h - 24.0f);
        dl->AddRectFilled(t0, t1, asset_kind_color(a.kind), 2.0f);
        const char* kindlbl = asset_kind_label(a.kind);
        ImVec2 ks = ImGui::CalcTextSize(kindlbl);
        dl->AddText(
            ImVec2(t0.x + ((t1.x - t0.x) - ks.x) * 0.5f,
                   t0.y + ((t1.y - t0.y) - ks.y) * 0.5f),
            IM_COL32(0, 0, 0, 180), kindlbl);

        // Label
        ImVec2 ls = ImGui::CalcTextSize(a.name.c_str());
        float maxw = tile_w - 6.0f;
        const char* draw_name = a.name.c_str();
        char shortened[64];
        if (ls.x > maxw) {
            int max_chars = (int)((maxw / ls.x) * a.name.size()) - 1;
            if (max_chars < 3) max_chars = 3;
            if (max_chars > (int)a.name.size()) max_chars = (int)a.name.size();
            std::snprintf(shortened, sizeof(shortened), "%.*s...", max_chars, a.name.c_str());
            draw_name = shortened;
            ls = ImGui::CalcTextSize(draw_name);
        }
        dl->AddText(
            ImVec2(p.x + (tile_w - ls.x) * 0.5f, p.y + tile_h - 18.0f),
            PC_TEXT, draw_name);

        ImGui::PopID();
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Status bar — body only. Caller wraps in BeginViewportSideBar so the bar
// pins to the bottom of the main viewport outside the dockspace.
// ---------------------------------------------------------------------------
static void DrawStatusBarContent()
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(" 0 errors  ·  0 warnings ");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("HibitEcs · main");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("entities: %d", (int)g_entities.size());

    float right_w = 180.0f;
    float avail   = ImGui::GetContentRegionAvail().x;
    if (avail > right_w) {
        ImGui::SameLine(ImGui::GetCursorPosX() + (avail - right_w));
    }
    ImGui::TextDisabled("FPS %.0f  ·  %.2f ms",
        ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point — DockSpace host + dockable panels.
//
// Layout: a status bar pins to the bottom of the main viewport via
// BeginViewportSideBar, then DockSpaceOverViewport fills the remainder. Each
// panel is its own ImGui window, freely re-arrangeable / tabbable by the user.
// On first run we use the DockBuilder API to seed the PlayCanvas layout
// (Hierarchy left, Inspector right, Assets bottom, Viewport center).
// ---------------------------------------------------------------------------
void DrawPlayCanvasEditor()
{
    seed_scene_once();

    // Status bar at the bottom of the main viewport (outside the dockspace).
    ImGui::PushStyleColor(ImGuiCol_WindowBg, PC_BG_DEEP);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, kStatusWindowPadding);
    if (ImGui::BeginViewportSideBar("##statusbar", ImGui::GetMainViewport(),
            ImGuiDir_Down, kStatusBarHeight,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings)) {
        DrawStatusBarContent();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // DockSpace fills the remaining viewport work area.
    const ImGuiID dockspace_id =
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    // Seed the default layout once. ImGui persists subsequent user tweaks to
    // imgui.ini, so re-runs honor the user's last arrangement.
    static bool layout_initialized = false;
    if (!layout_initialized) {
        layout_initialized = true;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id,
            ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id,
            ImGui::GetMainViewport()->WorkSize);

        ImGuiID center = dockspace_id;
        ImGuiID left   = ImGui::DockBuilderSplitNode(center,
                            ImGuiDir_Left,  0.20f, nullptr, &center);
        ImGuiID right  = ImGui::DockBuilderSplitNode(center,
                            ImGuiDir_Right, 0.25f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center,
                            ImGuiDir_Down,  0.30f, nullptr, &center);

        ImGui::DockBuilderDockWindow("Hierarchy",     left);
        ImGui::DockBuilderDockWindow("Script Editor", right);
        ImGui::DockBuilderDockWindow("Assets",        bottom);
        ImGui::DockBuilderDockWindow("Viewport",      center);
        ImGui::DockBuilderFinish(dockspace_id);
    }

    if (ImGui::Begin("Hierarchy")) {
        DrawHierarchy();
    }
    ImGui::End();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Viewport", nullptr,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        DrawViewport();
    }
    ImGui::End();
    ImGui::PopStyleVar();

    if (ImGui::Begin("Script Editor")) {
        DrawScriptEditor();
    }
    ImGui::End();

    if (ImGui::Begin("Assets")) {
        DrawAssets();
    }
    ImGui::End();
}
