/**
 * users_manager.h
 * ZASLON v2.4 — User Account Management
 */
#pragma once
#include <string>
#include <vector>

struct UserInfo {
    std::wstring Name;
    std::wstring Comment;
    bool         IsAdmin;
};

void UsersManager_Render();
void UsersManager_Refresh();
