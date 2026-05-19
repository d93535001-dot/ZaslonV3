#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <functional>

namespace Zaslon {
namespace UI {

// --------------------------------------------------------------------------
// Localization (i18n) Engine
// --------------------------------------------------------------------------
enum class Language {
    English,
    Russian
};

class Localization {
public:
    static Localization& GetInstance() {
        static Localization instance;
        return instance;
    }

    void SetLanguage(Language lang) { m_current_lang = lang; }
    Language GetLanguage() const { return m_current_lang; }

    const char* GetString(const char* key);

private:
    Localization();
    Language m_current_lang = Language::English;
    std::map<std::string, std::map<Language, std::string>> m_dictionary;
};

// --------------------------------------------------------------------------
// Theme & Customization Engine
// --------------------------------------------------------------------------
struct ThemeColors {
    float WindowBg[4];
    float Header[4];
    float HeaderHovered[4];
    float HeaderActive[4];
    float Button[4];
    float ButtonHovered[4];
    float ButtonActive[4];
    float Text[4];
    float TextDisabled[4];
    float Border[4];
    float Accent[4]; // Deep customization accent color (e.g., Cyberpunk Neon, Enterprise Blue)
};

struct ThemeSettings {
    ThemeColors colors;
    float windowRounding = 4.0f;
    float frameRounding = 2.0f;
    float scrollbarRounding = 9.0f;
    std::string fontFamily = "Consolas";
    float fontSize = 16.0f;
    bool enableTransparency = true; // Transparency effect
};

class ThemeManager {
public:
    static ThemeManager& GetInstance() {
        static ThemeManager instance;
        return instance;
    }

    void ApplyTheme(const ThemeSettings& settings);
    ThemeSettings GetCurrentTheme() const { return m_current_settings; }

    // Pre-defined stylish themes
    void LoadEnterpriseTheme();
    void LoadCyberDarkTheme();

private:
    ThemeManager() { LoadEnterpriseTheme(); }
    ThemeSettings m_current_settings;
};

// --------------------------------------------------------------------------
// UI Renderer Fallback Architecture
// --------------------------------------------------------------------------
enum class RendererType {
    ImGui_Direct3D9,    // Hardware Accelerated
    ImGui_OpenGL,       // Hardware Accelerated (Alternative)
    ImGui_SoftwareRasterizer, // Slow but guaranteed (WinPE without drivers)
    Classic_Win32_GDI   // Native Windows UI (Absolute fallback)
};

class IUIRenderer {
public:
    virtual ~IUIRenderer() = default;
    virtual bool Initialize() = 0;
    virtual void RenderFrame() = 0;
    virtual void Shutdown() = 0;
};

// --------------------------------------------------------------------------
// Layout Components (IDE-Style)
// --------------------------------------------------------------------------
class ViewComponent {
public:
    virtual ~ViewComponent() = default;
    virtual void Draw() = 0;
    virtual const char* GetName() const = 0;
    bool IsOpen = true;
};

// --------------------------------------------------------------------------
// UIManager: The Core Controller
// --------------------------------------------------------------------------
class UIManager {
public:
    static UIManager& GetInstance() {
        static UIManager instance;
        return instance;
    }

    // Attempts to initialize hardware renderer. Gracefully degrades to software/GDI if it fails.
    bool InitializeSmartFallback();
    void RunMainLoop();
    void Shutdown();

    // Docking layout management
    void RegisterView(std::shared_ptr<ViewComponent> view);
    void SetConsoleLogCallback(std::function<std::vector<std::string>()> callback);

private:
    UIManager() = default;
    void DrawIDEWindow(); // Draws the main DockSpace

    std::unique_ptr<IUIRenderer> m_renderer;
    RendererType m_active_renderer;

    std::vector<std::shared_ptr<ViewComponent>> m_views;
    std::function<std::vector<std::string>()> m_get_logs_callback;
};

} // namespace UI
} // namespace Zaslon
