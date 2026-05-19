/**
 * users_manager.cpp
 * ZASLON v2.4 — User Account Management
 *
 * Lists/creates/deletes local users via NetAPI32.
 * Useful for creating admin recovery accounts or resetting
 * passwords when locked out by malware.
 */
#include <windows.h>
#include "users_manager.h"
#include "imgui.h"
#include "gui_theme.h"
#include <lm.h>
#include <string>
#include <vector>

#pragma comment(lib, "netapi32.lib")

static std::vector<UserInfo> g_Users;
static bool g_Loaded = false;
static char g_StatusMsg[256] = {};
static bool g_StatusOk = true;

static void SetStatus(const char *msg, bool ok) {
  strncpy_s(g_StatusMsg, msg, sizeof(g_StatusMsg) - 1);
  g_StatusOk = ok;
}

// ── NetAPI wrappers ─────────────────────────────────────────────────────

void UsersManager_Refresh() {
  g_Users.clear();
  LPUSER_INFO_1 buf = nullptr;
  DWORD read = 0, total = 0, resume = 0;

  NET_API_STATUS st =
      NetUserEnum(nullptr, 1, FILTER_NORMAL_ACCOUNT, (LPBYTE *)&buf,
                  MAX_PREFERRED_LENGTH, &read, &total, &resume);

  if (st == NERR_Success && buf) {
    for (DWORD i = 0; i < read; i++) {
      UserInfo u;
      u.Name = buf[i].usri1_name ? buf[i].usri1_name : L"";
      u.Comment = buf[i].usri1_comment ? buf[i].usri1_comment : L"";
      u.IsAdmin = (buf[i].usri1_priv == USER_PRIV_ADMIN);
      g_Users.push_back(u);
    }
    NetApiBufferFree(buf);
    SetStatus("", true);
  } else {
    SetStatus(u8"Ошибка: не удалось получить список пользователей", false);
  }
  g_Loaded = true;
}

static bool ResetPassword(const std::wstring &username,
                          const std::wstring &newPass) {
  USER_INFO_1003 ui;
  ui.usri1003_password = (LPWSTR)newPass.c_str();
  NET_API_STATUS st =
      NetUserSetInfo(nullptr, username.c_str(), 1003, (LPBYTE)&ui, nullptr);
  return (st == NERR_Success);
}

static bool CreateUser(const std::wstring &username,
                       const std::wstring &password) {
  USER_INFO_1 ui = {};
  ui.usri1_name = (LPWSTR)username.c_str();
  ui.usri1_password = (LPWSTR)password.c_str();
  ui.usri1_priv = USER_PRIV_USER;
  ui.usri1_comment = (LPWSTR)L"Created by Machinist";
  ui.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT;

  NET_API_STATUS st = NetUserAdd(nullptr, 1, (LPBYTE)&ui, nullptr);
  if (st != NERR_Success)
    return false;

  // Add to Administrators group
  LOCALGROUP_MEMBERS_INFO_3 gm;
  gm.lgrmi3_domainandname = (LPWSTR)username.c_str();
  NetLocalGroupAddMembers(nullptr, L"Administrators", 3, (LPBYTE)&gm, 1);
  return true;
}

static bool DeleteUser(const std::wstring &username) {
  return NetUserDel(nullptr, username.c_str()) == NERR_Success;
}

// ── GUI ─────────────────────────────────────────────────────────────────

void UsersManager_Render() {
  ImGui::TextColored(g_Theme.AccentColor, u8"Управление пользователями");
  ImGui::TextWrapped(u8"Управление локальными учетными записями");
  ImGui::Separator();
  ImGui::Spacing();

  // ── Buttons row ─────────────────────────────────────────────────
  if (ImGui::Button(u8"Обновить список", ImVec2(160, 30)))
    UsersManager_Refresh();

  ImGui::SameLine(0, 10.0f);
  if (ImGui::Button(u8"Создать zaslon (пароль: 1)", ImVec2(250, 30))) {
    if (CreateUser(L"zaslon", L"1")) {
      SetStatus(u8"Пользователь 'zaslon' создан (администратор, пароль: 1)",
                true);
      UsersManager_Refresh();
    } else {
      SetStatus(u8"Ошибка создания (возможно, уже существует)", false);
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        u8"Создаёт локального администратора 'zaslon' с паролем '1'\n"
        u8"Используйте, если заблокированы из основной учётной записи");

  // Status message
  if (g_StatusMsg[0]) {
    ImGui::Spacing();
    ImVec4 col = g_StatusOk ? ImVec4(0.4f, 1.0f, 0.6f, 1.0f)
                            : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    ImGui::TextColored(col, "%s", g_StatusMsg);
  }

  if (!g_Loaded) {
    ImGui::Spacing();
    ImGui::TextDisabled(u8"Нажмите 'Обновить список' для начала.");
    return;
  }

  ImGui::Spacing();

  // ── Users table ─────────────────────────────────────────────────
  if (ImGui::BeginTable("UsersTable", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY,
                        ImVec2(0, 0))) {

    ImGui::TableSetupColumn(u8"Имя", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableSetupColumn(u8"Права", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn(u8"Описание", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(u8"Действия", ImGuiTableColumnFlags_WidthFixed,
                            220.0f);
    ImGui::TableHeadersRow();

    for (auto &u : g_Users) {
      ImGui::TableNextRow();

      // Name
      ImGui::TableNextColumn();
      char nb[128];
      WideCharToMultiByte(CP_UTF8, 0, u.Name.c_str(), -1, nb, 128, nullptr,
                          nullptr);
      ImGui::Text("%s", nb);

      // Privilege
      ImGui::TableNextColumn();
      if (u.IsAdmin)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Админ");
      else
        ImGui::TextDisabled("Юзер");

      // Comment
      ImGui::TableNextColumn();
      char cb[256];
      WideCharToMultiByte(CP_UTF8, 0, u.Comment.c_str(), -1, cb, 256, nullptr,
                          nullptr);
      ImGui::TextDisabled("%s", cb);

      // Actions
      ImGui::TableNextColumn();
      ImGui::PushID(nb);
      if (ImGui::SmallButton(u8"Пароль -> '1'")) {
        if (ResetPassword(u.Name, L"1")) {
          char msg[128];
          snprintf(msg, sizeof(msg), u8"Пароль '%s' сброшен на '1'", nb);
          SetStatus(msg, true);
        } else {
          SetStatus(u8"Ошибка сброса пароля", false);
        }
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"Сбросить пароль на '1'");

      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
      if (ImGui::SmallButton(u8"Удалить")) {
        if (DeleteUser(u.Name)) {
          char msg[128];
          snprintf(msg, sizeof(msg), u8"'%s' удалён", nb);
          SetStatus(msg, true);
          UsersManager_Refresh();
        } else {
          SetStatus(u8"Ошибка удаления", false);
        }
      }
      ImGui::PopStyleColor();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}
