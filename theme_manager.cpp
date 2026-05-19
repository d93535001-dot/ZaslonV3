#include "theme_manager.h"
#include <fstream>
#include <sstream>
#include <iostream>

std::unordered_map<std::string, std::string> g_CustomStrings;

const char* _(const char* defaultText) {
    auto it = g_CustomStrings.find(defaultText);
    if (it != g_CustomStrings.end()) {
        return it->second.c_str();
    }
    return defaultText;
}

namespace ThemeManager {

    static std::string WStringToString(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string result(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &result[0], size, NULL, NULL);
        return result;
    }

    static std::wstring StringToWString(const std::string& str) {
        if (str.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
        std::wstring result(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &result[0], size);
        return result;
    }

    bool ExportTheme(const std::wstring& filePath) {
        std::ofstream out(filePath);
        if (!out.is_open()) return false;

        out << "[ZaslonTheme]\n";
        out << "Version=2.5.3\n";
        
        auto saveColor = [&](const char* key, ImVec4 c) {
            out << key << "=" << c.x << "," << c.y << "," << c.z << "," << c.w << "\n";
        };

        saveColor("AccentColor", g_Theme.AccentColor);
        saveColor("WindowBg", g_Theme.WindowBg);
        saveColor("TextColor", g_Theme.TextColor);
        saveColor("TextDisabled", g_Theme.TextDisabled);
        saveColor("FrameBg", g_Theme.FrameBg);
        saveColor("HeaderBg", g_Theme.HeaderBg);
        saveColor("ButtonBg", g_Theme.ButtonBg);
        saveColor("PopupBg", g_Theme.PopupBg);
        saveColor("BorderColor", g_Theme.BorderColor);
        
        saveColor("BtnGrad1", g_Theme.BtnGrad1);
        saveColor("BtnGrad2", g_Theme.BtnGrad2);
        saveColor("NavGrad1", g_Theme.NavGrad1);
        saveColor("NavGrad2", g_Theme.NavGrad2);
        saveColor("BgGrad1", g_Theme.BgGrad1);
        saveColor("BgGrad2", g_Theme.BgGrad2);
        saveColor("PatternCol", g_Theme.PatternCol);

        out << "Minimalist=" << (g_Theme.Minimalist ? "1" : "0") << "\n";
        out << "AlwaysOnTop=" << (g_Theme.AlwaysOnTop ? "1" : "0") << "\n";
        out << "AdvancedColors=" << (g_Theme.AdvancedColors ? "1" : "0") << "\n";
        
        out << "UseBtnGrad=" << (g_Theme.UseBtnGrad ? "1" : "0") << "\n";
        out << "UseNavGrad=" << (g_Theme.UseNavGrad ? "1" : "0") << "\n";
        out << "UseBgGrad=" << (g_Theme.UseBgGrad ? "1" : "0") << "\n";
        out << "EnableShimmer=" << (g_Theme.EnableShimmer ? "1" : "0") << "\n";
        out << "PatternType=" << g_Theme.PatternType << "\n";
        out << "StealthMode=" << (g_Theme.StealthMode ? "1" : "0") << "\n";
        out << "CustomFontPath=" << WStringToString(g_Theme.CustomFontPath) << "\n";
        
        out << "\n[Dictionary]\n";
        for (const auto& kv : g_CustomStrings) {
            out << kv.first << "=" << kv.second << "\n";
        }

        return true;
    }

    bool ImportTheme(const std::wstring& filePath) {
        std::ifstream in(filePath);
        if (!in.is_open()) return false;

        std::string line;
        std::string currentSection = "";
        
        g_CustomStrings.clear();

        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';') continue;
            
            if (line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                continue;
            }

            size_t eqPos = line.find('=');
            if (eqPos == std::string::npos) continue;

            std::string key = line.substr(0, eqPos);
            std::string val = line.substr(eqPos + 1);

            if (currentSection == "ZaslonTheme") {
                auto parseColor = [&](ImVec4& c) {
                    sscanf_s(val.c_str(), "%f,%f,%f,%f", &c.x, &c.y, &c.z, &c.w);
                };

                if (key == "AccentColor") parseColor(g_Theme.AccentColor);
                else if (key == "WindowBg") parseColor(g_Theme.WindowBg);
                else if (key == "TextColor") parseColor(g_Theme.TextColor);
                else if (key == "TextDisabled") parseColor(g_Theme.TextDisabled);
                else if (key == "FrameBg") parseColor(g_Theme.FrameBg);
                else if (key == "HeaderBg") parseColor(g_Theme.HeaderBg);
                else if (key == "ButtonBg") parseColor(g_Theme.ButtonBg);
                else if (key == "PopupBg") parseColor(g_Theme.PopupBg);
                else if (key == "BorderColor") parseColor(g_Theme.BorderColor);
                else if (key == "BtnGrad1") parseColor(g_Theme.BtnGrad1);
                else if (key == "BtnGrad2") parseColor(g_Theme.BtnGrad2);
                else if (key == "NavGrad1") parseColor(g_Theme.NavGrad1);
                else if (key == "NavGrad2") parseColor(g_Theme.NavGrad2);
                else if (key == "BgGrad1") parseColor(g_Theme.BgGrad1);
                else if (key == "BgGrad2") parseColor(g_Theme.BgGrad2);
                else if (key == "PatternCol") parseColor(g_Theme.PatternCol);
                else if (key == "Minimalist") g_Theme.Minimalist = (val == "1");
                else if (key == "AlwaysOnTop") g_Theme.AlwaysOnTop = (val == "1");
                else if (key == "AdvancedColors") g_Theme.AdvancedColors = (val == "1");
                else if (key == "UseBtnGrad") g_Theme.UseBtnGrad = (val == "1");
                else if (key == "UseNavGrad") g_Theme.UseNavGrad = (val == "1");
                else if (key == "UseBgGrad") g_Theme.UseBgGrad = (val == "1");
                else if (key == "EnableShimmer") g_Theme.EnableShimmer = (val == "1");
                else if (key == "PatternType") g_Theme.PatternType = std::stoi(val);
                else if (key == "StealthMode") g_Theme.StealthMode = (val == "1");
                else if (key == "CustomFontPath") g_Theme.CustomFontPath = StringToWString(val);
            }
            else if (currentSection == "Dictionary") {
                g_CustomStrings[key] = val;
            }
        }
        
        ApplyZaslonTheme(g_Theme);
        SaveThemeSettings();
        return true;
    }

    void ReloadFonts() {
        // Handled in main.cpp ImGui setup, this just signals the need
    }

    bool CheckEmergencyReset() {
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            g_Theme = ZaslonTheme(); // basic defaults
            g_Theme.CustomFontPath = L"";
            g_CustomStrings.clear();
            SaveThemeSettings();
            return true;
        }
        return false;
    }
}
