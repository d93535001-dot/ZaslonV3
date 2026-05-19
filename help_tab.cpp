/**
 * help_tab.cpp
 * ZASLON v2.5 — Help & Version History
 *
 * Full changelog from v0.90 BETA through v2.5.
 */
#include "help_tab.h"
#include "imgui.h"
#include "gui_theme.h"
#include "svg_icons.h"
#include <cstdio>

// ── Section helper ──────────────────────────────────────────────────────

static void VersionSection(const char *version, const char *subtitle,
                           ImVec4 color, const char *body,
                           bool openByDefault = false, int iconId = -1) {
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x * 0.3f, color.y * 0.3f,
                                                color.z * 0.3f, 1.0f));
  char label[128];
  snprintf(label, sizeof(label), "%s — %s", version, subtitle);
  ImGuiTreeNodeFlags flags = openByDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0;

  // Custom header rendering to include SVG
  bool isOpen = ImGui::CollapsingHeader(label, flags);

  if (iconId >= 0) {
    ImVec2 minPos = ImGui::GetItemRectMin();
    ImVec2 maxPos = ImGui::GetItemRectMax();
    // Draw icon aligned to the right side of the header
    ImVec2 iconPos = ImVec2(maxPos.x - 24.0f,
                            minPos.y + (maxPos.y - minPos.y - 16.0f) * 0.5f);
    ZaslonGUI::DrawIcon(iconId, iconPos, 16.0f,
                        ImGui::ColorConvertFloat4ToU32(color));
  }

  if (isOpen) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.88f, 1.0f));
    ImGui::Indent(12.0f);
    ImGui::TextWrapped("%s", body);
    ImGui::Unindent(12.0f);
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  ImGui::PopStyleColor();
}

// ── Main render ─────────────────────────────────────────────────────────

void HelpTab_Render() {
  ImGui::TextColored(g_Theme.AccentColor, u8"ZASLON — Справка");
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::TextWrapped(u8"ZASLON — утилита хорошая, наверное... "
                     u8"История обновлений:");
  ImGui::Spacing();
  ImGui::Spacing();

  // ─── v2.7.0 ───────────────────────────────────────────────────────
  VersionSection("v2.7.0", u8"Фикс", ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                 u8"Основное:\n"
                 u8"  - переработка тем и добавление градиентов\n"
                 u8"  - прямая починка системных файлов мертвой ОС с флешки "
                 u8"(Ну это удобно)\n"
                 u8"  - ремонт загрузчика, чинит MBR, Boot и BCD\n"
                 u8"  - удаление говна из автозагрузки, до запуска винды\n"
                 u8"  - оффлайн-редактор теперь сам подставляет пути до "
                 u8"системных файлов\n\n",
                 true, ZaslonGUI::ICON_DASHBOARD);

  // ─── v2.6.0 ───────────────────────────────────────────────────────
  VersionSection(
      "v2.6.0", u8"Редизайн", ImVec4(0.2f, 0.8f, 0.4f, 1.0f),
      u8"Основное:\n"
      u8"  - с нуля переписана функция трассировщика\n"
      u8"  - трассировщик вынесен в отдельную вкладку 'Установщик'\n"
      u8"  - шрифтовые иконки заменены на полноформатные векторные свг иконки\n"
      u8"  - реструктуризация файлов и чистка архитектуры\n\n",
      false, ZaslonGUI::ICON_INSTALLER);

  // ─── v2.5.3 ───────────────────────────────────────────────────────
  VersionSection(
      "v2.5.3", u8"Доп. к 2.5", ImVec4(1.0f, 0.4f, 0.6f, 1.0f),
      u8"Основное:\n"
      u8"  - полная поддержка внешних тем (.ztheme)\n"
      u8"  - сканирование и удаление IFEO перехватов по всему реестру\n"
      u8"  - 12 новых пиздатых тем\n"
      u8"  - поддержка кастомных TTF шрифтов\n",
      false, ZaslonGUI::ICON_SETTINGS);

  // ─── v2.5 ───────────────────────────────────────────────────────
  VersionSection(
      "v2.5", u8"Оптимизация+переработка", ImVec4(1.0f, 0.4f, 0.6f, 1.0f),
      u8"Основное:\n"
      u8"  - расширенная настройка интерфейса\n"
      u8"  - возможность ручной настройки 9 цветов всех элементов UI (Текст, "
      u8"Кнопки, Рамки и тд)\n"
      u8"  - автосканирование в дашборде при запуске\n"
      u8"  - проверка отключённый или включённый Windows Update и открытый "
      u8"RDP\n"
      u8"  - подтверждение при нажатии 'Исправить всё'\n"
      u8"  - индикатор количества процессов и угроз в тулбаре\n\n"
      u8"Фиксы:\n"
      u8"  - расчёт расстояния оптимизирован\n"
      u8"  - устранена утечка COM объекта D3D9\n"
      u8"  - очистка истории CPU\n"
      u8"  - лимит загрузки иконок в 8 потоков\n",
      false);

  // ─── v2.4 ───────────────────────────────────────────────────────
  VersionSection("v2.4", u8"Новность", ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                 u8"Основное:\n"
                 u8"  - дашборд\n"
                 u8"  - сканер IFEO Debugger, DisallowRun\n"
                 u8"  - менеджер пользователей\n\n"
                 u8"Гуишка:\n"
                 u8"  - полностью переработан навбар\n"
                 u8"  - подсветка активной вкладки\n"
                 u8"  - настройка 'Поверх всех окон'\n"
                 u8"  - кеширование системной информации в статус-баре\n\n"
                 u8"Исправления:\n"
                 u8"  - исправлены ошибки сборки\n"
                 u8"  - полный отчет в справке",
                 false);

  // ─── v2.3 ───────────────────────────────────────────────────────
  VersionSection(
      "v2.3", u8"асинхронность", ImVec4(0.3f, 0.9f, 0.5f, 1.0f),
      u8"  - поддержка WinPE (начало реализации)\n"
      u8"  - асинхронные операции: Winsock/BCD сброс в фоновом потоке\n"
      u8"  - навбар с иконками и адаптивной шириной кнопок\n"
      u8"  - единая система тем\n"
      u8"  - настройки сохраняются в zaslon_ui.ini\n");

  // ─── v2.2 ───────────────────────────────────────────────────────
  VersionSection("v2.2", u8"Защита", ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                 u8"  - изолированный рабочий стол(БЕТА)\n"
                 u8"  - фоновый мониторинг TopMost окон\n"
                 u8"  - вакцинация USB — autorun.inf\n"
                 u8"  - сканирование EFI/ESP раздела\n"
                 u8"  - песочница");

  // ─── v2.1 ───────────────────────────────────────────────────────
  VersionSection("v2.1", u8"Офлайн", ImVec4(0.8f, 0.6f, 1.0f, 1.0f),
                 u8"  - редактор реестра\n"
                 u8"  - сброс пароля пользователя\n"
                 u8"  - управление службами\n"
                 u8"  - патч залипания клавиш");

  // ─── v2.0 ───────────────────────────────────────────────────────
  VersionSection("v2.0", u8"безопасность", ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                 u8"  - терминатор процессов\n"
                 u8"  - эвристический движок оценки рисков\n"
                 u8"  - детектор фейковых системных процессов\n"
                 u8"  - разблокировка файлов\n"
                 u8"  - удаление файлов при перезагрузке\n"
                 u8"  - сброс политик SRP/Safer");

  // ─── v1.8 ───────────────────────────────────────────────────────
  VersionSection("v1.8", u8"Автозагрузка", ImVec4(0.5f, 0.9f, 0.9f, 1.0f),
                 u8"  - сканер автозагрузки\n"
                 u8"  - сканер планировщика задач\n"
                 u8"  - детектор бесфайловых вирусов\n"
                 u8"  - удаление вредоносных WMI скриптов");

  // ─── v1.5 ───────────────────────────────────────────────────────
  VersionSection("v1.5", u8"Сеть", ImVec4(0.6f, 0.8f, 0.4f, 1.0f),
                 u8"  - сброс файла HOSTS\n"
                 u8"  - сброс стека Winsock/TCP-IP\n"
                 u8"  - отключение SMBv1\n"
                 u8"  - включение отображения расширений файлов\n"
                 u8"  - разблокировка UAC и Defender");

  // ─── v1.2 ───────────────────────────────────────────────────────
  VersionSection("v1.2", u8"Фиксы", ImVec4(0.9f, 0.7f, 0.5f, 1.0f),
                 u8"  - проверка целостности: SHA-256 хеши sethc/utilman\n"
                 u8"  - обход TrustedInstaller для замены системных файлов\n"
                 u8"  - очистка IFEO перехватов\n"
                 u8"  - восстановление Shell и Userinit в реестре");

  // ─── v1.0 ───────────────────────────────────────────────────────
  VersionSection("v1.0", u8"Фиксы+обнова", ImVec4(0.5f, 0.7f, 1.0f, 1.0f),
                 u8"  - файловый менеджер-проводник\n"
                 u8"  - базовый убийца процессов\n"
                 u8"  - разблокировка диспетчера задач и regedit\n"
                 u8"  - восстановление системных шрифтов");

  // ─── v0.90 BETA ─────────────────────────────────────────────────
  VersionSection("v0.90 BETA", u8"Начало", ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                 u8"  - каркас приложения: ImGui + Direct3D9\n"
                 u8"  - прототип менеджера процессов\n"
                 u8"  - эскалация привилегий (runas)");

  // ─── Documentation sections ─────────────────────────────────────
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(g_Theme.AccentColor, u8"Руководство по модулям");
  ImGui::Spacing();

  auto RenderModuleDoc = [](const char *title, const char *body, int iconId) {
    bool isOpen = ImGui::CollapsingHeader(title);
    ImVec2 minPos = ImGui::GetItemRectMin();
    ImVec2 maxPos = ImGui::GetItemRectMax();
    ImVec2 iconPos = ImVec2(maxPos.x - 24.0f,
                            minPos.y + (maxPos.y - minPos.y - 16.0f) * 0.5f);
    ZaslonGUI::DrawIcon(iconId, iconPos, 16.0f,
                        ImGui::ColorConvertFloat4ToU32(g_Theme.AccentColor));
    if (isOpen) {
      ImGui::Indent(12.0f);
      ImGui::TextWrapped("%s", body);
      ImGui::Unindent(12.0f);
      ImGui::Spacing();
    }
  };

  RenderModuleDoc(
      u8"Панель мониторинга",
      u8"выполняет 7 базовых проверок системы: "
      u8"блокировки диспетчера задач и реестра, подмена shell/userinit, "
      u8"IFEO перехваты, статус HOSTS, скрытые расширения "
      u8"каждая проблема фикситься удобно",
      ZaslonGUI::ICON_DASHBOARD);

  RenderModuleDoc(u8"Процессы",
                  u8"отображает процессы с оценкой угрозы 0-100%% "
                  u8"жесткого снятия защищенных процессов "
                  u8"автодетекция фейковых системных файлов и TopMost-баннеров",
                  ZaslonGUI::ICON_PROCESSES);

  RenderModuleDoc(u8"Файлы",
                  u8"проводник без участия explorer.exe "
                  u8"обход блокировок файлов"
                  u8"удаление через movefileex при перезагрузке",
                  ZaslonGUI::ICON_FILES);

  RenderModuleDoc(u8"Ограничения",
                  u8"сканер IFEO Debugger, DisallowRun политик "
                  u8"подмен раскладки клавиатуры. "
                  u8"кнопка 'Снять все' сносит блокировки из реестра",
                  ZaslonGUI::ICON_RESTRICTIONS);

  RenderModuleDoc(u8"Пользователи",
                  u8"управление локальными аккаунтами. Сброс "
                  u8"пароля на '1' или бэкдор-создание админа 'zaslon' "
                  u8"для восстановления доступа к учетке",
                  ZaslonGUI::ICON_USERS);

  RenderModuleDoc(u8"Установщик",
                  u8"монитор установки программ. "
                  u8"отслеживает новые файлы, ключи реестра, процессы и кражу "
                  u8"откат мусора",
                  ZaslonGUI::ICON_INSTALLER);

  if (ImGui::CollapsingHeader(u8"Горячие клавиши")) {
    ImVec2 minPos = ImGui::GetItemRectMin();
    ImVec2 maxPos = ImGui::GetItemRectMax();
    ImVec2 iconPos = ImVec2(maxPos.x - 24.0f,
                            minPos.y + (maxPos.y - minPos.y - 16.0f) * 0.5f);
    ZaslonGUI::DrawIcon(ZaslonGUI::ICON_HELP, iconPos, 16.0f,
                        ImGui::ColorConvertFloat4ToU32(g_Theme.AccentColor));
    ImGui::TextWrapped(
        u8"Ctrl + Alt + Z — вызов изолированного рабочего стола");
  }
}
