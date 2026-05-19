/**
 * main.cpp
 * ZASLON — Emergency Recovery Utility (User Mode)
 *
 * Win32 Entry point, ImGui+D3D9 setup, main render loop.
 *
 * Developer: Machinist
 */
#include "gui_theme.h"
#include "help_tab.h"

// Macro cleanup for ntstatus redefinitions
#define WIN32_NO_STATUS
#include "resource.h"
#include <ntstatus.h>
#include <string>
#include <vector>
#include <windows.h>
#include <winternl.h>


#undef WIN32_NO_STATUS

// Helper to extract embedded DLLs
static bool ExtractResourceToFile(int resourceId,
                                  const std::wstring &outputPath) {
  HRSRC hRes = FindResourceW(GetModuleHandleW(nullptr),
                             MAKEINTRESOURCEW(resourceId), (LPCWSTR)RT_RCDATA);
  if (!hRes)
    return false;
  HGLOBAL hData = LoadResource(GetModuleHandleW(nullptr), hRes);
  if (!hData)
    return false;
  DWORD size = SizeofResource(GetModuleHandleW(nullptr), hRes);
  void *pData = LockResource(hData);
  if (!pData)
    return false;
  HANDLE hFile = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  WriteFile(hFile, pData, size, &written, nullptr);
  CloseHandle(hFile);
  return (written == size);
}

// Ensure all critical dependencies are available (extract from resources if
// missing or needed for WinPE)
static void BootstrapDependencies() {
  wchar_t tempPath[MAX_PATH];
  GetTempPathW(MAX_PATH, tempPath);

  // Add temp path to DLL search order for delay loading
  SetDllDirectoryW(tempPath);

  struct Dependency {
    int id;
    std::wstring name;
    bool critical;
  };

  std::vector<Dependency> deps = {{IDR_D3D9_DLL, L"d3d9.dll", true},
                                  {IDR_D3D8THK_DLL, L"d3d8thk.dll", true},
                                  {IDR_RSTRTMGR_DLL, L"Rstrtmgr.dll", false},
                                  {IDR_IPHLPAPI_DLL, L"iphlpapi.dll", false},
                                  {IDR_NETAPI32_DLL, L"netapi32.dll", false}};

  bool anyFailure = false;
  for (auto &dep : deps) {
    std::wstring fullPath = std::wstring(tempPath) + dep.name;

    // Always attempt extraction in WinPE or if file is missing
    if (!ExtractResourceToFile(dep.id, fullPath)) {
      if (dep.critical)
        anyFailure = true;
      continue;
    }

    // Try to pre-load it to ensure it's valid
  // FIX: Use LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
  // to prevent arbitrary DLL hijacking from the temp directory if sideloaded.
    HMODULE hMod = LoadLibraryExW(fullPath.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!hMod && dep.critical) {
      anyFailure = true;
    }
  }

  if (anyFailure) {
    MessageBoxW(nullptr,
                L"КРИТИЧЕСКАЯ ОШИБКА: Не удалось подготовить компоненты "
                L"(DirectX/System). Программа может не запуститься.",
                L"ZASLON Error", MB_ICONERROR | MB_OK);
  }
}

#include <d3d9.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tchar.h>

#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "imgui.h"

#include "autorun_scanner.h"
#include "dashboard.h"
#include "file_explorer.h"
#include "gui_utils.h"
#include "gui_utils.h" // Needed for embedded fonts
#include "installer_tracker.h"
#include "integrity_module.h"
#include "offline_os_manager.h"
#include "offline_registry.h"
#include "process_panel.h"
#include "restrictions_manager.h"
#include "svg_icons.h"
#include "system_repair.h"
#include "theme_manager.h" // v2.5.3 Theme Manager
#include "users_manager.h"
#include "zaslon_core.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

// D3D globals
static LPDIRECT3D9 g_pD3D = nullptr;
LPDIRECT3DDEVICE9 g_pd3dDevice = nullptr;
static D3DPRESENT_PARAMETERS g_d3dpp = {};

bool CreateDeviceD3D(HWND hWnd) {
  if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr)
    return false;

  ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
  g_d3dpp.Windowed = TRUE;
  g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
  g_d3dpp.EnableAutoDepthStencil = TRUE;
  g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
  g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE; // VSync on

  // Pass 1: Try Hardware Vertex Processing (Normal OS)
  if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                           D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp,
                           &g_pd3dDevice) < 0) {
    // Pass 2: Fallback to Software Vertex Processing (WinPE / Basic Display
    // Driver)
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                             D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_d3dpp,
                             &g_pd3dDevice) < 0) {
      // Pass 3: Ultimate Fallback (Reference Device - incredibly slow, but
      // might show the window)
      if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hWnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_d3dpp,
                               &g_pd3dDevice) < 0) {
        g_pD3D->Release();
        g_pD3D = nullptr;
        return false;
      }
    }
  }

  return true;
}

void CleanupDeviceD3D() {
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
  if (g_pD3D) {
    g_pD3D->Release();
    g_pD3D = nullptr;
  }
}

void ResetDevice() {
  if (!g_pd3dDevice)
    return;
  ImGui_ImplDX9_InvalidateDeviceObjects();
  HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
  if (hr == D3DERR_INVALIDCALL)
    IM_ASSERT(0);
  ImGui_ImplDX9_CreateDeviceObjects();
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    return true;

  switch (msg) {
  case WM_SIZE:
    if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
      g_d3dpp.BackBufferWidth = LOWORD(lParam);
      g_d3dpp.BackBufferHeight = HIWORD(lParam);
      ResetDevice();
    }
    return 0;
  case WM_SYSCOMMAND:
    if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
      return 0;
    break;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  case WM_HOTKEY:
    if (wParam == 1) { // Ctrl+Alt+Z
      ZaslonCore::AntiWinLocker::LaunchIsolatedDesktop(L"cmd.exe");
    }
    break;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Detect Windows PE environment via MiniNT registry key
static bool IsRunningInWinPE() {
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\MiniNT", 0, KEY_READ,
                    &hKey) == ERROR_SUCCESS) {
    RegCloseKey(hKey);
    return true;
  }
  return false;
}

static void EnableDebugPrivilege() {
  HANDLE hToken;
  if (OpenProcessToken(GetCurrentProcess(),
                       TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
      AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(hToken);
  }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nShowCmd) {
  // Ensure all dependencies are available (critical for WinPE)
  BootstrapDependencies();

  wchar_t exePath[MAX_PATH];
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);

  // Detect WinPE environment early
  g_IsWinPE = IsRunningInWinPE();
  if (g_IsWinPE) {
    if (ZaslonCore::OfflineOSManager::DetectOfflineOS()) {
      ZaslonCore::OfflineOSManager::MountOfflineHives();
    }
  }

  // Strict Admin Rights Enforcement (skip in WinPE — already SYSTEM)
  if (!g_IsWinPE) {
    BOOL fIsRunAsAdmin = FALSE;
    PSID pAdministratorsGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                 &pAdministratorsGroup)) {
      CheckTokenMembership(nullptr, pAdministratorsGroup, &fIsRunAsAdmin);
      FreeSid(pAdministratorsGroup);
    }

    if (!fIsRunAsAdmin) {
      SHELLEXECUTEINFOW sei = {sizeof(sei)};
      sei.lpVerb = L"runas";
      sei.lpFile = exePath;
      sei.nShow = nShowCmd;
      if (ShellExecuteExW(&sei))
        return 0; // Successfully elevated

      MessageBoxW(nullptr, L"ДЛЯ РАБОТЫ ТРЕБУЮТСЯ ПРАВА АДМИНИСТРАТОРА!",
                  L"Критическая Ошибка", MB_ICONERROR | MB_OK);
      return 1;
    }
  }

  // High DPI awareness
  SetProcessDPIAware();
  EnableDebugPrivilege();

  // ZASLON GUI Self-Defense: We removed strict MS-only mitigation to prevent
  // "Bad Image" popups on user systems with custom themes like
  // ExplorerBlurMica. Instead we rely on Hunter Mode and Anti-WinLocker logic
  // internally.

  WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc,     0L,
                    0L,         hInstance,  nullptr,     nullptr,
                    nullptr,    nullptr,    L"ZaslonWc", nullptr};
  RegisterClassExW(&wc);

  int sx = GetSystemMetrics(SM_CXSCREEN);
  int sy = GetSystemMetrics(SM_CYSCREEN);
  int wx = 1350;
  int wy = 900;

  HWND hwnd = CreateWindowW(wc.lpszClassName, L"ZASLON - MachinistX",
                            WS_OVERLAPPEDWINDOW, (sx - wx) / 2, (sy - wy) / 2,
                            wx, wy, nullptr, nullptr, wc.hInstance, nullptr);

  if (!CreateDeviceD3D(hwnd)) {
    // If all D3D9 initialization completely fails, we have no GUI fallback
    // implemented yet, so we must notify the user somehow before exiting
    // silently.
    if (g_IsWinPE) {
      MessageBoxW(nullptr, L"D3D9 Failed. WinPE lacks DirectX acceleration.",
                  L"ZASLON WinPE Warning", MB_ICONERROR | MB_OK);
    }
    CleanupDeviceD3D();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  ShowWindow(hwnd, nShowCmd);
  UpdateWindow(hwnd);

  // Phase 11: Register Global Hotkey for Anti-WinLocker
  RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT, 0x5A); // Z

  // Phase 11: Start Anti-WinLocker Guardian
  ZaslonCore::AntiWinLocker::StartWinLockerGuardian();

  // Initialise ImGui
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  // v2.5.3: Emergency Escape Hatch
  ThemeManager::CheckEmergencyReset();

  LoadThemeSettings();
  ApplyZaslonTheme(g_Theme);

  // Load High-Quality UI Font (Segoe UI) with extended Cyrillic and Symbol
  // ranges
  static const ImWchar ranges[] = {
      0x0020, 0x00FF, // Basic Latin
      0x0400, 0x052F, // Cyrillic
      0x2000, 0x206F, // General Punctuation
      0x2190, 0x21FF, // Arrows
      0x25A0, 0x25FF, // Geometric Shapes
      0x2600, 0x26FF, // Miscellaneous Symbols (Warning signs, gears, etc)
      0x2700, 0x27BF, // Dingbats
      0,
  };

  ImFontConfig fontConfig;
  fontConfig.OversampleH = 2;
  fontConfig.OversampleV = 2;

  // ZASLON GUI Self-Defense: If virus deleted system fonts or running in WinPE,
  // use embedded font.
  bool customFontLoaded = false;

  // v2.5.3: Load Custom Font if requested
  if (!g_Theme.CustomFontPath.empty()) {
    char fontPathMb[MAX_PATH] = {0};
    WideCharToMultiByte(CP_UTF8, 0, g_Theme.CustomFontPath.c_str(), -1,
                        fontPathMb, MAX_PATH, NULL, NULL);
    ImFont *customFont =
        io.Fonts->AddFontFromFileTTF(fontPathMb, 18.0f, &fontConfig, ranges);
    if (customFont)
      customFontLoaded = true;
  }

  // Regular OS fonts fallback (ONLY IF NOT IN WINPE AND NO CUSTOM FONT)
  if (!customFontLoaded && !g_IsWinPE) {
    if (io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f,
                                     &fontConfig, ranges) ||
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 18.0f,
                                     &fontConfig, ranges)) {
      customFontLoaded = true;
    }
  }

  // Ultimate fallback: embedded bytes (Crucial for WinPE to prevent
  // box-characters or crashes)
  if (!customFontLoaded) {
    LoadRussianFont(io);
  } else {
    io.Fonts->Build();
  }

  // Customization is now handled entirely by ApplyZaslonTheme()
  // Note: LoadRussianFont() removed — primary font above already covers
  // the Cyrillic range (0x0400-0x052F), adding it again would create a
  // conflicting duplicate default font.

  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX9_Init(g_pd3dDevice);

  int currentTab = 0;
  bool done = false;

  while (!done) {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      if (msg.message == WM_QUIT)
        done = true;
    }
    if (done)
      break;

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // AlwaysOnTop toggle
    static bool s_lastOnTop = false;
    if (g_Theme.AlwaysOnTop != s_lastOnTop) {
      SetWindowPos(hwnd, g_Theme.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
                   0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
      s_lastOnTop = g_Theme.AlwaysOnTop;
    }

    // Stealth Mode (Exclude from Screen Capture)
    static bool s_lastStealth = false;
    if (g_Theme.StealthMode != s_lastStealth) {
      // WDA_EXCLUDEFROMCAPTURE = 0x11 (Windows 10 2004+)
      // WDA_MONITOR = 1 (Older fallback)
      SetWindowDisplayAffinity(hwnd, g_Theme.StealthMode ? 0x00000011 : 0);
      s_lastStealth = g_Theme.StealthMode;
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("ZASLON Dashboard", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (g_Theme.UseBgGrad) {
        ImVec2 p_min = ImVec2(0, 0);
        ImVec2 p_max = io.DisplaySize;
        
        ImVec4 c1 = g_Theme.BgGrad1;
        ImVec4 c2 = g_Theme.BgGrad2;
        c1.w *= g_Theme.BgGradAlpha;
        c2.w *= g_Theme.BgGradAlpha;
        
        ImU32 col1 = ImGui::ColorConvertFloat4ToU32(c1);
        ImU32 col2 = ImGui::ColorConvertFloat4ToU32(c2);

        // Premium Diagonal Gradient (Top-Left to Bottom-Right)
        ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(
            p_min, p_max,
            col1, // Top-Left
            col2, // Top-Right
            col1, // Bottom-Right
            col2  // Bottom-Left
        );
    }

    if (g_Theme.PatternType > 0) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImVec2 size = io.DisplaySize;
        ImU32 col = ImGui::ColorConvertFloat4ToU32(g_Theme.PatternCol);
        float spacing = 25.0f;
        if (g_Theme.PatternType == 1) { // Dots
            for (float x = spacing/2; x < size.x; x += spacing) {
                for (float y = spacing/2; y < size.y; y += spacing) {
                    drawList->AddCircleFilled(ImVec2(x, y), 1.0f, col);
                }
            }
        } else if (g_Theme.PatternType == 2) { // Grid
            for (float x = 0; x < size.x; x += spacing) {
                drawList->AddLine(ImVec2(x, 0), ImVec2(x, size.y), col);
            }
            for (float y = 0; y < size.y; y += spacing) {
                drawList->AddLine(ImVec2(0, y), ImVec2(size.x, y), col);
            }
        }
    }

    // Top Navbar — v2.5 clean design
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 8.0f));
    ImGui::BeginChild("##navbar", ImVec2(0, 44), true, ImGuiWindowFlags_NoScrollbar);

    if (g_Theme.UseNavGrad) {
      ImVec2 p_min = ImGui::GetCursorScreenPos();
      ImVec2 p_max = ImVec2(p_min.x + ImGui::GetContentRegionAvail().x + 20.0f,
                             p_min.y + 44.0f);
      // Non-linear corner gradient
      ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
          p_min, p_max, 
          ImGui::ColorConvertFloat4ToU32(g_Theme.NavGrad1),
          ImGui::ColorConvertFloat4ToU32(g_Theme.NavGrad2),
          ImGui::ColorConvertFloat4ToU32(g_Theme.NavGrad2),
          ImGui::ColorConvertFloat4ToU32(g_Theme.NavGrad1));
    }

    // Active tab styling with accent underline
    auto NavButton = [&](const char *label, int tabIndex, int iconId) {
      ImGui::PushID(tabIndex);
      bool isActive = (currentTab == tabIndex);

      ImVec4 iconCol = ImVec4(0.70f, 0.70f, 0.72f, 1.0f);

      if (isActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, g_Theme.AccentColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_Theme.AccentColor);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        iconCol = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.20f, 0.20f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.72f, 1.0f));
      }
      ImVec2 textSize = ImGui::CalcTextSize(label);
      float btnWidth = textSize.x + 35.0f; // Extra space for icon

      ImVec2 cursorPos = ImGui::GetCursorScreenPos();
      if (ImGui::Button("##hide", ImVec2(btnWidth, 28))) {
        currentTab = tabIndex;
      }
      bool isHovered = ImGui::IsItemHovered();
      if (!isActive && isHovered) {
        iconCol = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
      }

      // Draw text
      ImVec2 textPos = ImVec2(cursorPos.x + 28.0f,
                              cursorPos.y + (28.0f - textSize.y) * 0.5f);
      ImGui::GetWindowDrawList()->AddText(
          textPos,
          ImGui::GetColorU32(
              isActive ? ImGuiCol_Text
                       : (isHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled)),
          label);

      // Draw Icon
      ImVec2 iconPos = ImVec2(cursorPos.x + 8.0f, cursorPos.y + 6.0f);
      ZaslonGUI::DrawIcon(iconId, iconPos, 16.0f,
                          ImGui::ColorConvertFloat4ToU32(iconCol));

      ImGui::PopStyleColor(3);
      ImGui::PopID();
    };

    // Tabs using translation wrapper _()
    NavButton(_(u8"Обзор"), 0, ZaslonGUI::ICON_DASHBOARD);
    ImGui::SameLine();
    NavButton(_(u8"Процессы"), 1, ZaslonGUI::ICON_PROCESSES);
    ImGui::SameLine();
    NavButton(_(u8"Файлы"), 2, ZaslonGUI::ICON_FILES);
    ImGui::SameLine();
    NavButton(_(u8"Система"), 3, ZaslonGUI::ICON_SYSTEM);
    ImGui::SameLine();
    NavButton(_(u8"Реестр"), 4, ZaslonGUI::ICON_REGISTRY);
    ImGui::SameLine();
    NavButton(_(u8"Автозагрузка"), 5, ZaslonGUI::ICON_AUTORUN);
    ImGui::SameLine();
    NavButton(_(u8"Восстановление"), 6, ZaslonGUI::ICON_RECOVERY);
    ImGui::SameLine();
    NavButton(_(u8"Ограничения"), 7, ZaslonGUI::ICON_RESTRICTIONS);
    ImGui::SameLine();
    NavButton(_(u8"Пользователи"), 8, ZaslonGUI::ICON_USERS);
    ImGui::SameLine();
    NavButton(_(u8"Справка"), 9, ZaslonGUI::ICON_HELP);
    ImGui::SameLine();
    NavButton(_(u8"Настройки"), 10, ZaslonGUI::ICON_SETTINGS);
    ImGui::SameLine();
    NavButton(_(u8"Установщик"), 11, ZaslonGUI::ICON_INSTALLER);

    // Top right text removed to avoid overlapping 'Installer' tab

    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Spacing();

    // Main Workspace (leave room for status bar)
    float statusBarHeight = 28.0f;
    ImGui::BeginChild(
        "##workspace",
        ImVec2(0, -statusBarHeight - ImGui::GetStyle().ItemSpacing.y), false);
    switch (currentTab) {
    case 0:
      Dashboard_Render();
      break;
    case 1:
      ProcessPanel_Render();
      break;
    case 2:
      FileExplorer_Render();
      break;
    case 3:
      IntegrityModule_Render();
      break;
    case 4:
      OfflineRegistry_Render();
      break;
    case 5:
      AutorunScanner_Render();
      break;
    case 6:
      SystemRepair_Render();
      break;
    case 7:
      RestrictionsManager_Render();
      break;
    case 8:
      UsersManager_Render();
      break;
    case 9:
      HelpTab_Render();
      break;
    case 10:
      ThemeSettings_Render();
      break;
    case 11:
      InstallerTracker_Render();
      break;
    }
    ImGui::EndChild();

    // Status Bar
    ImGui::Separator();
    ImGui::BeginChild("##statusbar", ImVec2(0, statusBarHeight), false);
    {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
      ImGui::Text("ZASLON v2.7.0");
      
      if (g_Theme.StealthMode) {
        ImGui::SameLine(0, 20.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), u8"[ НЕВИДИМОСТЬ ]");
      }
      ImGui::SameLine(0, 20.0f);

      if (g_IsWinPE) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        if (ZaslonCore::g_IsOfflineHivesMounted) {
          char pathUtf8[MAX_PATH] = {0};
          WideCharToMultiByte(CP_UTF8, 0, ZaslonCore::g_OfflineOSPath.c_str(),
                              -1, pathUtf8, MAX_PATH, NULL, NULL);
          ImGui::Text(u8"[WinPE] %s", pathUtf8);
        } else {
          ImGui::Text(u8"[WinPE]");
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 20.0f);
      }

      // Uptime
      ULONGLONG uptimeMs = GetTickCount64();
      unsigned int uptimeSec = (unsigned int)(uptimeMs / 1000);
      unsigned int hours = uptimeSec / 3600;
      unsigned int minutes = (uptimeSec % 3600) / 60;
      ImGui::Text(u8"Uptime: %uh %um", hours, minutes);

      // System info: OS version + total RAM (cached per session)
      static DWORD s_osMajor = 0, s_osMinor = 0, s_osBuild = 0;
      static ULONGLONG s_totalRamMB = 0;
      static bool s_infoLoaded = false;
      if (!s_infoLoaded) {
        OSVERSIONINFOW osvi = {sizeof(osvi)};
        typedef NTSTATUS(NTAPI * pfnRtlGetVersion)(PRTL_OSVERSIONINFOW);
        pfnRtlGetVersion fn = (pfnRtlGetVersion)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        if (fn) {
          fn((PRTL_OSVERSIONINFOW)&osvi);
          s_osMajor = osvi.dwMajorVersion;
          s_osMinor = osvi.dwMinorVersion;
          s_osBuild = osvi.dwBuildNumber;
        }
        MEMORYSTATUSEX ms = {sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        s_totalRamMB = ms.ullTotalPhys / (1024 * 1024);
        s_infoLoaded = true;
      }

      ImGui::SameLine(0, 20.0f);
      ImGui::Text(u8"Win %u.%u.%u", s_osMajor, s_osMinor, s_osBuild);
      ImGui::SameLine(0, 15.0f);
      ImGui::Text(u8"RAM: %llu MB", s_totalRamMB);

      ImGui::SameLine(ImGui::GetWindowWidth() - 200);
      ImGui::Text(u8"MachinistX");
      ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::End();

    // Rendering
    ImGui::EndFrame();
    g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

    // Sync D3D background with Theme
    D3DCOLOR clear_col_dx = D3DCOLOR_RGBA(
        (int)(g_Theme.WindowBg.x * 255.0f), (int)(g_Theme.WindowBg.y * 255.0f),
        (int)(g_Theme.WindowBg.z * 255.0f), 255);

    g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                        clear_col_dx, 1.0f, 0);

    if (g_pd3dDevice->BeginScene() >= 0) {
      ImGui::Render();
      ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
      g_pd3dDevice->EndScene();
    }
    HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
    if (result == D3DERR_DEVICELOST &&
        g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
      ResetDevice();
  }

  if (g_IsWinPE) {
    ZaslonCore::OfflineOSManager::UnmountOfflineHives();
  }

  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  // Unregister Ctrl+Alt+Z hotkey to avoid OS resource leak
  UnregisterHotKey(hwnd, 1);

  CleanupDeviceD3D();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);

  return 0;
}
