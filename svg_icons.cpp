#include "svg_icons.h"
#include <cmath>

namespace ZaslonGUI {

// Utility to draw a circle
static void DrawCircle(ImDrawList* draw_list, ImVec2 center, float radius, ImU32 col, float thickness) {
    draw_list->AddCircle(center, radius, col, 0, thickness);
}

// Utility to draw an ellipse
static void DrawEllipse(ImDrawList* draw_list, ImVec2 center, float rx, float ry, ImU32 col, float thickness) {
    draw_list->AddEllipse(center, ImVec2(rx, ry), col, 0.0f, 0, thickness);
}

// Utility to draw a rectangle with rounded corners
static void DrawRectRounded(ImDrawList* draw_list, ImVec2 p_min, ImVec2 p_max, float rounding, ImU32 col, float thickness) {
    draw_list->AddRect(p_min, p_max, col, rounding, 0, thickness);
}

// Utility to draw a line
static void DrawLine(ImDrawList* draw_list, ImVec2 p1, ImVec2 p2, ImU32 col, float thickness) {
    draw_list->AddLine(p1, p2, col, thickness);
}

void DrawIcon(int iconId, ImVec2 pos, float size, ImU32 color) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    // Scale factor based on standard 24x24 viewBox
    float scale = size / 24.0f;
    float stroke_width = 2.0f * scale;

    auto p = [pos, scale](float x, float y) -> ImVec2 {
        return ImVec2(pos.x + x * scale, pos.y + y * scale);
    };

    switch (iconId) {
        case ICON_DASHBOARD: {
            // <rect x="3" y="3" width="7" height="9" rx="1"></rect>
            // <rect x="14" y="3" width="7" height="5" rx="1"></rect>
            // <rect x="14" y="12" width="7" height="9" rx="1"></rect>
            // <rect x="3" y="16" width="7" height="5" rx="1"></rect>
            DrawRectRounded(draw_list, p(3, 3), p(10, 12), 1.0f * scale, color, stroke_width);
            DrawRectRounded(draw_list, p(14, 3), p(21, 8), 1.0f * scale, color, stroke_width);
            DrawRectRounded(draw_list, p(14, 12), p(21, 21), 1.0f * scale, color, stroke_width);
            DrawRectRounded(draw_list, p(3, 16), p(10, 21), 1.0f * scale, color, stroke_width);
            break;
        }
        case ICON_PROCESSES: {
            // <path d="M22 12h-4l-3 9L9 3l-3 9H2"></path>
            draw_list->PathLineTo(p(22, 12));
            draw_list->PathLineTo(p(18, 12));
            draw_list->PathLineTo(p(15, 21));
            draw_list->PathLineTo(p(9, 3));
            draw_list->PathLineTo(p(6, 12));
            draw_list->PathLineTo(p(2, 12));
            draw_list->PathStroke(color, 0, stroke_width);
            break;
        }
        case ICON_FILES: {
            draw_list->PathArcTo(p(4, 5), 2.0f * scale, 3.1415f, 3.1415f * 1.5f);
            draw_list->PathLineTo(p(9, 3));
            draw_list->PathLineTo(p(11, 6));
            draw_list->PathArcTo(p(20, 8), 2.0f * scale, -3.1415f / 2, 0.0f);
            draw_list->PathArcTo(p(20, 19), 2.0f * scale, 0.0f, 3.1415f / 2);
            draw_list->PathArcTo(p(4, 19), 2.0f * scale, 3.1415f / 2, 3.1415f);
            draw_list->PathStroke(color, ImDrawFlags_Closed, stroke_width);
            break;
        }
        case ICON_SYSTEM: {
            // <rect x="2" y="3" width="20" height="14" rx="2" ry="2"></rect>
            // <line x1="8" y1="21" x2="16" y2="21"></line>
            // <line x1="12" y1="17" x2="12" y2="21"></line>
            DrawRectRounded(draw_list, p(2, 3), p(22, 17), 2.0f * scale, color, stroke_width);
            DrawLine(draw_list, p(8, 21), p(16, 21), color, stroke_width);
            DrawLine(draw_list, p(12, 17), p(12, 21), color, stroke_width);
            break;
        }
        case ICON_REGISTRY: {
            // Clean Registry Cylinders (Wireframe)
            DrawEllipse(draw_list, p(12, 5), 8.0f * scale, 2.5f * scale, color, stroke_width);
            DrawEllipse(draw_list, p(12, 12), 8.0f * scale, 2.5f * scale, color, stroke_width);
            DrawEllipse(draw_list, p(12, 19), 8.0f * scale, 2.5f * scale, color, stroke_width);
            
            DrawLine(draw_list, p(4, 5), p(4, 19), color, stroke_width);
            DrawLine(draw_list, p(20, 5), p(20, 19), color, stroke_width);
            break;
        }
        case ICON_AUTORUN: {
            // <polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"></polygon>
            ImVec2 points[] = { p(13,2), p(3,14), p(12,14), p(11,22), p(21,10), p(12,10) };
            draw_list->AddPolyline(points, 6, color, ImDrawFlags_Closed, stroke_width);
            break;
        }
        case ICON_RECOVERY: {
            draw_list->PathArcTo(p(12, 12), 9.0f * scale, -3.1415f*0.75f, 3.1415f);
            draw_list->PathStroke(color, 0, stroke_width);
            
            draw_list->PathLineTo(p(3, 3));
            draw_list->PathLineTo(p(3, 8));
            draw_list->PathLineTo(p(8, 8));
            draw_list->PathStroke(color, 0, stroke_width);
            break;
        }
        case ICON_RESTRICTIONS: {
            // Clear padlock for Restrictions
            DrawRectRounded(draw_list, p(6, 11), p(18, 20), 2.0f * scale, color, stroke_width);
            
            // Shackle (Top half circle, PI to 0 or manual lines to avoid ArcTo inversion issue)
            draw_list->PathLineTo(p(8, 11));
            draw_list->PathLineTo(p(8, 8));
            draw_list->PathArcTo(p(12, 8), 4.0f * scale, -3.1415f, 0.0f, 16);
            draw_list->PathLineTo(p(16, 11));
            draw_list->PathStroke(color, 0, stroke_width);
            
            // Keyhole
            DrawCircle(draw_list, p(12, 14.5f), 1.0f * scale, color, stroke_width);
            DrawLine(draw_list, p(12, 15.5f), p(12, 17.5f), color, stroke_width);
            break;
        }
        case ICON_USERS: {
            // User 2 (Back/Right, partial silhouette so it doesn't cross over)
            DrawCircle(draw_list, p(17, 10), 2.5f * scale, color, stroke_width);
            draw_list->PathArcTo(p(17, 22), 5.0f * scale, -3.1415f/2.0f, 0.0f, 8); // Right shoulder
            draw_list->PathStroke(color, 0, stroke_width);

            // User 1 (Front/Left)
            DrawCircle(draw_list, p(10, 8), 3.5f * scale, color, stroke_width);
            // Full top half circle from -PI to 0
            draw_list->PathArcTo(p(10, 22), 7.0f * scale, -3.1415f, 0.0f, 16);
            draw_list->PathStroke(color, 0, stroke_width);
            break;
        }
        case ICON_HELP: {
            // Info Icon
            DrawCircle(draw_list, p(12, 12), 10.0f * scale, color, stroke_width);
            draw_list->AddCircleFilled(p(12, 7.5f), 1.25f * scale, color);
            DrawLine(draw_list, p(12, 10.5f), p(12, 17.0f), color, stroke_width);
            DrawLine(draw_list, p(10.5f, 17.0f), p(13.5f, 17.0f), color, stroke_width);
            break;
        }
        case ICON_SETTINGS: {
            DrawCircle(draw_list, p(12, 12), 3.0f * scale, color, stroke_width);
            const int teeth = 8;
            for (int i = 0; i < teeth; i++) {
                float a1 = (i * 45 - 12) * 3.1415f / 180.0f;
                float a2 = (i * 45 + 12) * 3.1415f / 180.0f;
                float a3 = (i * 45 + 24) * 3.1415f / 180.0f;
                float a4 = ((i+1) * 45 - 24) * 3.1415f / 180.0f;
                
                draw_list->PathLineTo(p(12 + cosf(a1)*9.0f, 12 + sinf(a1)*9.0f));
                draw_list->PathLineTo(p(12 + cosf(a2)*9.0f, 12 + sinf(a2)*9.0f));
                draw_list->PathLineTo(p(12 + cosf(a3)*6.5f, 12 + sinf(a3)*6.5f));
                draw_list->PathLineTo(p(12 + cosf(a4)*6.5f, 12 + sinf(a4)*6.5f));
            }
            draw_list->PathStroke(color, ImDrawFlags_Closed, stroke_width);
            break;
        }
        case ICON_INSTALLER: {
            // <path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"></path>
            // <polyline points="3.27 6.96 12 12.01 20.73 6.96"></polyline>
            // <line x1="12" y1="22.08" x2="12" y2="12"></line>
            ImVec2 box[] = {
                p(12, 2), p(20.5f, 6.5f), p(20.5f, 16.5f), p(12, 21), p(3.5f, 16.5f), p(3.5f, 6.5f)
            };
            draw_list->AddPolyline(box, 6, color, ImDrawFlags_Closed, stroke_width);
            
            DrawLine(draw_list, p(3.5f, 6.5f), p(12, 12), color, stroke_width);
            DrawLine(draw_list, p(20.5f, 6.5f), p(12, 12), color, stroke_width);
            DrawLine(draw_list, p(12, 21), p(12, 12), color, stroke_width);
            break;
        }
    }
}

} // namespace ZaslonGUI
