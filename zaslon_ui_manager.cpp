#include "zaslon_ui_manager.h"
#include "zaslon_logger.h"
#include <iostream>

namespace Zaslon {
namespace UI {

// --------------------------------------------------------------------------
// Localization
// --------------------------------------------------------------------------
Localization::Localization() {
    // Basic dictionary setup (No legacy 'Help' clutter)
    m_dictionary["App_Title"] = { {Language::English, "ZASLON 3.0.1a"}, {Language::Russian, "ЗАСЛОН 3.0.1a"} };
    m_dictionary["Menu_File"] = { {Language::English, "File"}, {Language::Russian, "Файл"} };
    m_dictionary["Menu_View"] = { {Language::English, "View"}, {Language::Russian, "Вид"} };
    m_dictionary["Menu_Settings"] = { {Language::English, "Settings"}, {Language::Russian, "Настройки"} };
    m_dictionary["Tab_Dashboard"] = { {Language::English, "Dashboard"}, {Language::Russian, "Дашборд"} };
    m_dictionary["Tab_Process"] = { {Language::English, "Process Explorer"}, {Language::Russian, "Менеджер Процессов"} };
    m_dictionary["Tab_Console"] = { {Language::English, "Telemetry Console"}, {Language::Russian, "Консоль Телеметрии"} };
    m_dictionary["Btn_Kill"] = { {Language::English, "Force Kill"}, {Language::Russian, "Уничтожить"} };
}

const char* Localization::GetString(const char* key) {
    if (m_dictionary.count(key) && m_dictionary[key].count(m_current_lang)) {
        return m_dictionary[key][m_current_lang].c_str();
    }
    return key; // Fallback to key
}

// --------------------------------------------------------------------------
// Theme Manager
// --------------------------------------------------------------------------
void ThemeManager::ApplyTheme(const ThemeSettings& settings) {
    m_current_settings = settings;
    ZLOG_INFO("UI Theme applied. Transparent: {}, Accent: ({},{},{},{})",
        settings.enableTransparency, settings.colors.Accent[0], settings.colors.Accent[1], settings.colors.Accent[2], settings.colors.Accent[3]);

    // In a real ImGui implementation, this is where we push styles:
    // ImGuiStyle& style = ImGui::GetStyle();
    // style.WindowRounding = settings.windowRounding;
    // style.Colors[ImGuiCol_WindowBg] = ImVec4(settings.colors.WindowBg...);
}

void ThemeManager::LoadEnterpriseTheme() {
    ThemeSettings s;
    float wbg[] = {0.1f, 0.1f, 0.12f, 0.95f};
    std::copy(std::begin(wbg), std::end(wbg), std::begin(s.colors.WindowBg));
    float h[] = {0.2f, 0.2f, 0.25f, 1.0f};

    std::copy(std::begin(h), std::end(h), std::begin(s.colors.Header));
    float a[] = {0.0f, 0.47f, 0.84f, 1.0f};
    std::copy(std::begin(a), std::end(a), std::begin(s.colors.Accent));
    s.windowRounding = 2.0f;
    s.enableTransparency = false; // Solid for enterprise
    ApplyTheme(s);
}

void ThemeManager::LoadCyberDarkTheme() {
    ThemeSettings s;
    float wbg[] = {0.05f, 0.05f, 0.05f, 0.85f};
    std::copy(std::begin(wbg), std::end(wbg), std::begin(s.colors.WindowBg));
    float h[] = {0.15f, 0.0f, 0.2f, 1.0f};

    std::copy(std::begin(h), std::end(h), std::begin(s.colors.Header));
    float a[] = {0.8f, 0.0f, 0.4f, 1.0f};
    std::copy(std::begin(a), std::end(a), std::begin(s.colors.Accent));
    s.windowRounding = 6.0f;
    s.enableTransparency = true;
    ApplyTheme(s);
}

// --------------------------------------------------------------------------
// Renderer Stubs (Conceptual implementations)
// --------------------------------------------------------------------------
class ImGuiD3DRenderer : public IUIRenderer {
public:
    bool Initialize() override {
        ZLOG_INFO("Attempting Direct3D 9 Initialization...");
        // Simulate failure in WinPE environments
        return false;
    }
    void RenderFrame() override {}
    void Shutdown() override {}
};

class ImGuiSoftwareRenderer : public IUIRenderer {
public:
    bool Initialize() override {
        ZLOG_INFO("Attempting ImGui Software Rasterizer Initialization...");
        return true;
    }
    void RenderFrame() override {
        // ImGui::Render(); software_draw_data(...)
    }
    void Shutdown() override {}
};

class ClassicWin32Renderer : public IUIRenderer {
public:
    bool Initialize() override {
        ZLOG_INFO("Initializing Native Win32 GDI UI Fallback...");
        return true;
    }
    void RenderFrame() override { /* Native Message Pump */ }
    void Shutdown() override {}
};

// --------------------------------------------------------------------------
// UIManager
// --------------------------------------------------------------------------
bool UIManager::InitializeSmartFallback() {
    ZLOG_INFO("UIManager: Starting Smart Fallback Initialization.");

    // 1. Try D3D9 (Fastest, prettiest, but requires drivers)
    m_renderer = std::make_unique<ImGuiD3DRenderer>();
    if (m_renderer->Initialize()) {
        m_active_renderer = RendererType::ImGui_Direct3D9;
        return true;
    }

    // 2. Try Software ImGui (Maintains IDE style, but slower)
    m_renderer = std::make_unique<ImGuiSoftwareRenderer>();
    if (m_renderer->Initialize()) {
        m_active_renderer = RendererType::ImGui_SoftwareRasterizer;
        return true;
    }

    // 3. Absolute Fallback (Native Windows controls)
    m_renderer = std::make_unique<ClassicWin32Renderer>();
    if (m_renderer->Initialize()) {
        m_active_renderer = RendererType::Classic_Win32_GDI;
        return true;
    }

    ZLOG_CRIT("UIManager: ALL renderers failed to initialize.");
    return false;
}

void UIManager::RegisterView(std::shared_ptr<ViewComponent> view) {
    m_views.push_back(view);
}

void UIManager::DrawIDEWindow() {
    // Pseudocode for ImGui Docking layout
    auto& loc = Localization::GetInstance();

    // std::cout << "Rendering Window: " << loc.GetString("App_Title") << std::endl;
    // ImGui::SetNextWindowSize(ImVec2(1280, 720));
    // ImGui::Begin(loc.GetString("App_Title"), nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking);

    // ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    // ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    /*
    Conceptual Layout Building:
    if (first_time) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_id_bottom = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down, 0.25f, NULL, &dock_main_id);
        ImGuiID dock_id_left = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, 0.20f, NULL, &dock_main_id);

        ImGui::DockBuilderDockWindow(loc.GetString("Tab_Console"), dock_id_bottom);
        ImGui::DockBuilderDockWindow("Navigation", dock_id_left);
        ImGui::DockBuilderDockWindow(loc.GetString("Tab_Process"), dock_main_id);
        ImGui::DockBuilderFinish(dockspace_id);
    }
    */

    for (auto& view : m_views) {
        if (view->IsOpen) {
            // ImGui::Begin(view->GetName(), &view->IsOpen);
            view->Draw();
            // ImGui::End();
        }
    }

    // ImGui::End();
}

void UIManager::RunMainLoop() {
    ZLOG_INFO("UIManager: Starting main loop using renderer type {}", static_cast<int>(m_active_renderer));
    // while (!quit) { m_renderer->RenderFrame(); DrawIDEWindow(); }

    // Simulation execution
    DrawIDEWindow();
}

void UIManager::Shutdown() {
    if (m_renderer) {
        m_renderer->Shutdown();
    }
    ZLOG_INFO("UIManager: Shutdown complete.");
}

} // namespace UI
} // namespace Zaslon
