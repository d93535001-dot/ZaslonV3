#include "zaslon_logger.h"
#include "zaslon_ui_manager.h"
#include <iostream>

using namespace Zaslon::UI;
using namespace Zaslon::Foundation;

class DummyProcessView : public ViewComponent {
public:
    void Draw() override {
        // ImGui::Text("Process List Here");
    }
    const char* GetName() const override {
        return Localization::GetInstance().GetString("Tab_Process");
    }
};

class DummyConsoleView : public ViewComponent {
public:
    void Draw() override {
        // ImGui::Text("Logs Here");
    }
    const char* GetName() const override {
        return Localization::GetInstance().GetString("Tab_Console");
    }
};

int main() {
    auto& logger = Logger::GetInstance();
    logger.InitializeFileSink(L"zaslon_ui_test.log");

    std::cout << "Testing UIManager Architecture..." << std::endl;

    auto& ui = UIManager::GetInstance();
    auto& loc = Localization::GetInstance();
    auto& theme = ThemeManager::GetInstance();

    // Test Localization
    loc.SetLanguage(Language::Russian);
    std::cout << "App Title (RU): " << loc.GetString("App_Title") << std::endl;
    loc.SetLanguage(Language::English);
    std::cout << "App Title (EN): " << loc.GetString("App_Title") << std::endl;

    // Test Theming
    theme.LoadCyberDarkTheme();

    // Test Fallback Initialization
    if (ui.InitializeSmartFallback()) {
        std::cout << "UI Initialized successfully. Simulating window drawing..." << std::endl;

        ui.RegisterView(std::make_shared<DummyProcessView>());
        ui.RegisterView(std::make_shared<DummyConsoleView>());

        ui.RunMainLoop();
        ui.Shutdown();
    } else {
        std::cerr << "UI Init failed." << std::endl;
    }

    std::cout << "UI Tests complete. Check logs." << std::endl;
    return 0;
}
