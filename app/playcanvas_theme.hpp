#pragma once

#include <imgui.h>

// Approximation of the PlayCanvas editor visual style:
//   * Deep teal-gray neutrals (#1E2326 / #283034 / #2E363A)
//   * Signature PlayCanvas orange accent (#FF6600)
//   * Compact spacing (4–6 px), 2 px corner radii, 1 px hairline borders
//   * Flat tabs with an accent overline on the selected tab
//
// Pair with a Roboto / Source Sans body font at ~14 px for the closest
// visual match (see main.cpp font loader).
inline void ApplyPlayCanvasStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    const ImVec4 PC_BG_DEEP   = ImVec4(0.118f, 0.137f, 0.149f, 1.00f); // #1E2326
    const ImVec4 PC_BG        = ImVec4(0.157f, 0.184f, 0.196f, 1.00f); // #283034
    const ImVec4 PC_PANEL     = ImVec4(0.180f, 0.212f, 0.224f, 1.00f); // #2E363A
    const ImVec4 PC_PANEL_HL  = ImVec4(0.220f, 0.255f, 0.271f, 1.00f); // #384145
    const ImVec4 PC_BORDER    = ImVec4(0.075f, 0.090f, 0.098f, 1.00f); // #131719
    const ImVec4 PC_TEXT      = ImVec4(0.918f, 0.918f, 0.918f, 1.00f); // #EAEAEA
    const ImVec4 PC_TEXT_DIM  = ImVec4(0.553f, 0.580f, 0.592f, 1.00f); // #8D9497
    const ImVec4 PC_ACCENT    = ImVec4(1.000f, 0.400f, 0.000f, 1.00f); // #FF6600
    const ImVec4 PC_ACCENT_HV = ImVec4(1.000f, 0.490f, 0.157f, 1.00f); // #FF7D28
    const ImVec4 PC_ACCENT_AC = ImVec4(0.851f, 0.337f, 0.000f, 1.00f); // #D95600

    c[ImGuiCol_Text]                       = PC_TEXT;
    c[ImGuiCol_TextDisabled]               = PC_TEXT_DIM;
    c[ImGuiCol_WindowBg]                   = PC_BG;
    c[ImGuiCol_ChildBg]                    = PC_BG;
    c[ImGuiCol_PopupBg]                    = PC_PANEL;
    c[ImGuiCol_Border]                     = PC_BORDER;
    c[ImGuiCol_BorderShadow]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]                    = PC_BG_DEEP;
    c[ImGuiCol_FrameBgHovered]             = PC_PANEL_HL;
    c[ImGuiCol_FrameBgActive]              = PC_PANEL;
    c[ImGuiCol_TitleBg]                    = PC_BG_DEEP;
    c[ImGuiCol_TitleBgActive]              = PC_BG_DEEP;
    c[ImGuiCol_TitleBgCollapsed]           = PC_BG_DEEP;
    c[ImGuiCol_MenuBarBg]                  = PC_BG_DEEP;
    c[ImGuiCol_ScrollbarBg]                = PC_BG_DEEP;
    c[ImGuiCol_ScrollbarGrab]              = PC_PANEL;
    c[ImGuiCol_ScrollbarGrabHovered]       = PC_PANEL_HL;
    c[ImGuiCol_ScrollbarGrabActive]        = PC_ACCENT;
    c[ImGuiCol_CheckMark]                  = PC_ACCENT;
    c[ImGuiCol_SliderGrab]                 = PC_ACCENT;
    c[ImGuiCol_SliderGrabActive]           = PC_ACCENT_AC;
    c[ImGuiCol_Button]                     = PC_PANEL;
    c[ImGuiCol_ButtonHovered]              = PC_PANEL_HL;
    c[ImGuiCol_ButtonActive]               = PC_ACCENT;
    c[ImGuiCol_Header]                     = PC_PANEL;
    c[ImGuiCol_HeaderHovered]              = PC_PANEL_HL;
    c[ImGuiCol_HeaderActive]               = PC_ACCENT;
    c[ImGuiCol_Separator]                  = PC_BORDER;
    c[ImGuiCol_SeparatorHovered]           = PC_ACCENT_HV;
    c[ImGuiCol_SeparatorActive]            = PC_ACCENT;
    c[ImGuiCol_ResizeGrip]                 = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]          = PC_ACCENT_HV;
    c[ImGuiCol_ResizeGripActive]           = PC_ACCENT;
    c[ImGuiCol_Tab]                        = PC_BG_DEEP;
    c[ImGuiCol_TabHovered]                 = PC_PANEL_HL;
    c[ImGuiCol_TabSelected]                = PC_PANEL;
    c[ImGuiCol_TabSelectedOverline]        = PC_ACCENT;
    c[ImGuiCol_TabDimmed]                  = PC_BG_DEEP;
    c[ImGuiCol_TabDimmedSelected]          = PC_PANEL;
    c[ImGuiCol_TabDimmedSelectedOverline]  = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PlotLines]                  = PC_ACCENT;
    c[ImGuiCol_PlotLinesHovered]           = PC_ACCENT_HV;
    c[ImGuiCol_PlotHistogram]              = PC_ACCENT;
    c[ImGuiCol_PlotHistogramHovered]       = PC_ACCENT_HV;
    c[ImGuiCol_TableHeaderBg]              = PC_BG_DEEP;
    c[ImGuiCol_TableBorderStrong]          = PC_BORDER;
    c[ImGuiCol_TableBorderLight]           = PC_BORDER;
    c[ImGuiCol_TableRowBg]                 = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]              = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TextSelectedBg]             = ImVec4(1.00f, 0.40f, 0.00f, 0.35f);
    c[ImGuiCol_DragDropTarget]             = PC_ACCENT;
    c[ImGuiCol_NavCursor]                  = PC_ACCENT;
    c[ImGuiCol_NavWindowingHighlight]      = PC_ACCENT_HV;
    c[ImGuiCol_NavWindowingDimBg]          = ImVec4(0, 0, 0, 0.40f);
    c[ImGuiCol_ModalWindowDimBg]           = ImVec4(0, 0, 0, 0.55f);

    s.WindowPadding      = ImVec2(12, 10);
    s.FramePadding       = ImVec2(7, 4);
    s.CellPadding        = ImVec2(7, 4);
    s.ItemSpacing        = ImVec2(8, 6);
    s.ItemInnerSpacing   = ImVec2(6, 4);
    s.TouchExtraPadding  = ImVec2(0, 0);
    s.IndentSpacing      = 18.0f;
    s.ScrollbarSize      = 10.0f;
    s.GrabMinSize        = 8.0f;

    s.WindowBorderSize   = 1.0f;
    s.ChildBorderSize    = 1.0f;
    s.PopupBorderSize    = 1.0f;
    s.FrameBorderSize    = 0.0f;
    s.TabBorderSize      = 0.0f;
    s.TabBarBorderSize   = 1.0f;

    s.WindowRounding     = 2.0f;
    s.ChildRounding      = 2.0f;
    s.FrameRounding      = 2.0f;
    s.PopupRounding      = 2.0f;
    s.ScrollbarRounding  = 2.0f;
    s.GrabRounding       = 2.0f;
    s.TabRounding        = 2.0f;

    s.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    s.ButtonTextAlign          = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign      = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ColorButtonPosition      = ImGuiDir_Right;
}
