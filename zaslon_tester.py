import os
import sys
import ctypes
import winreg
import shutil
import time

def is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

def run_as_admin():
    if is_admin():
        return
    # Перезапуск с правами администратора
    try:
        ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, " ".join(f'"{a}"' if ' ' in a else a for a in sys.argv), None, 1)
    except Exception as e:
        print(f"Ошибка при запросе прав: {e}")
    sys.exit()

class ZaslonTester:
    def __init__(self):
        self.results = []

    def set_reg_value(self, root, path, name, value, value_type=winreg.REG_SZ):
        try:
            key = winreg.CreateKeyEx(root, path, 0, winreg.KEY_SET_VALUE)
            winreg.SetValueEx(key, name, 0, value_type, value)
            winreg.CloseKey(key)
            return True
        except Exception as e:
            print(f"Ошибка при установке {path}\\{name}: {e}")
            return False

    def delete_reg_key(self, root, path):
        try:
            winreg.DeleteKey(root, path)
            return True
        except FileNotFoundError:
            return True
        except Exception as e:
            print(f"Ошибка при удалении раздела {path}: {e}")
            return False

    def get_reg_value(self, root, path, name):
        try:
            key = winreg.OpenKey(root, path, 0, winreg.KEY_READ)
            value, _ = winreg.QueryValueEx(key, name)
            winreg.CloseKey(key)
            return value
        except:
            return None

    # --- Функции "Порчи" (Sabotage) ---
    def break_taskmgr(self):
        print("[!] Блокировка Диспетчера задач...")
        self.set_reg_value(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Policies\System", "DisableTaskMgr", 1, winreg.REG_DWORD)
        self.set_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System", "DisableTaskMgr", 1, winreg.REG_DWORD)
        # IFEO Hijack
        self.set_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe", "Debugger", "cmd.exe /c echo Диспетчер задач перехвачен тестером ZASLON & pause")

    def break_regedit(self):
        print("[!] Блокировка Редактора реестра...")
        self.set_reg_value(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Policies\System", "DisableRegistryTools", 1, winreg.REG_DWORD)

    def break_uac(self):
        print("[!] Отключение UAC (LUA)...")
        self.set_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System", "EnableLUA", 0, winreg.REG_DWORD)

    def break_shell(self):
        print("[!] Подмена оболочки (Shell) на Блокнот...")
        self.set_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon", "Shell", "notepad.exe")

    def break_sticky_keys(self):
        print("[!] Перехват залипания клавиш (sethc.exe)...")
        self.set_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\sethc.exe", "Debugger", "cmd.exe")

    def break_hosts(self):
        print("[!] Заражение файла HOSTS...")
        hosts_path = os.path.join(os.environ['WINDIR'], 'System32', 'drivers', 'etc', 'hosts')
        try:
            content = ""
            if os.path.exists(hosts_path):
                with open(hosts_path, 'r') as f:
                    content = f.read()
            
            if "google.com" not in content:
                with open(hosts_path, 'a') as f:
                    f.write("\n127.0.0.1 google.com # Malware Simulation\n")
        except Exception as e:
            print(f"Не удалось изменить hosts: {e}")

    def break_file_ext(self):
        print("[!] Скрытие расширений файлов...")
        self.set_reg_value(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced", "HideFileExt", 1, winreg.REG_DWORD)

    # --- Функции Проверки ---
    def verify_all(self):
        print("\n--- ОТЧЁТ О ПРОВЕРКЕ СИСТЕМЫ (ZASLON) ---")
        
        # Диспетчер задач
        tm_cu = self.get_reg_value(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Policies\System", "DisableTaskMgr")
        tm_ifeo = self.get_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe", "Debugger")
        tm_status = "ИСПРАВЛЕНО (БЕЗОПАСНО)" if (tm_cu != 1 and tm_ifeo is None) else "СЛОМАНО (ОПАСНО)"
        print(f"Диспетчер задач:   {tm_status}")

        # Редактор реестра
        re_cu = self.get_reg_value(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Policies\System", "DisableRegistryTools")
        re_status = "ИСПРАВЛЕНО (БЕЗОПАСНО)" if re_cu != 1 else "СЛОМАНО (ОПАСНО)"
        print(f"Редактор реестра:  {re_status}")

        # UAC
        uac = self.get_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System", "EnableLUA")
        uac_status = "ИСПРАВЛЕНО (БЕЗОПАСНО)" if uac == 1 else "СЛОМАНО (ОПАСНО)"
        print(f"UAC (Контроль уч.): {uac_status}")

        # Shell
        shell = self.get_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon", "Shell")
        shell_status = "ИСПРАВЛЕНО (БЕЗОПАСНО)" if shell == "explorer.exe" or shell is None else f"СЛОМАНО ({shell})"
        print(f"Оболочка (Shell):  {shell_status}")

        # Sticky Keys
        sethc = self.get_reg_value(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\sethc.exe", "Debugger")
        sk_status = "ИСПРАВЛЕНО (БЕЗОПАСНО)" if sethc is None else f"СЛОМАНО (Перехват: {sethc})"
        print(f"Залипание клавиш:  {sk_status}")

        # Hosts
        hosts_path = os.path.join(os.environ['WINDIR'], 'System32', 'drivers', 'etc', 'hosts')
        hosts_content = ""
        try:
            if os.path.exists(hosts_path):
                with open(hosts_path, 'r') as f:
                    hosts_content = f.read()
        except: pass
        hosts_status = "СЛОМАНО (Заражён)" if "google.com" in hosts_content else "ИСПРАВЛЕНО (БЕЗОПАСНО)"
        print(f"Файл HOSTS:        {hosts_status}")

        # Расширения
        hide_ext = self.get_reg_value(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced", "HideFileExt")
        ext_status = "ИСПРАВЛЕНО (ВИДИМЫ)" if hide_ext == 0 else "СЛОМАНО (СКРЫТЫ)"
        print(f"Расширения файлов: {ext_status}")
        
        print("-" * 40)

    def run_menu(self):
        while True:
            os.system('cls' if os.name == 'nt' else 'clear')
            print("========================================")
            print("    ZASLON: СТРЕСС-ТЕСТЕР СИСТЕМЫ")
            print("========================================")
            print("1. СЛОМАТЬ ВСЁ (Имитация заражения)")
            print("2. ПРОВЕРИТЬ ВСЁ (Статус исправления)")
            print("-" * 40)
            print("3. Заблокировать Диспетчер задач")
            print("4. Подменить Shell (на Блокнот)")
            print("5. Испортить файл HOSTS")
            print("6. Скрыть расширения файлов")
            print("-" * 40)
            print("0. Выход")
            
            try:
                choice = input("\nВыберите действие: ")
            except EOFError:
                break
            
            if choice == "1":
                self.break_taskmgr()
                self.break_regedit()
                self.break_uac()
                self.break_shell()
                self.break_sticky_keys()
                self.break_hosts()
                self.break_file_ext()
                print("\n[!!!] СИСТЕМА ПОВРЕЖДЕНА. Используйте ZASLON для исправления.")
                input("\nНажмите Enter для продолжения...")
            elif choice == "2":
                self.verify_all()
                input("\nНажмите Enter для продолжения...")
            elif choice == "3":
                self.break_taskmgr()
                input("\nГотово. Нажмите Enter...")
            elif choice == "4":
                self.break_shell()
                input("\nГотово. Нажмите Enter...")
            elif choice == "5":
                self.break_hosts()
                input("\nГотово. Нажмите Enter...")
            elif choice == "6":
                self.break_file_ext()
                input("\nГотово. Нажмите Enter...")
            elif choice == "0":
                break
            else:
                print("Неверный выбор.")
                time.sleep(1)

if __name__ == "__main__":
    if not is_admin():
        print("Запрос прав администратора...")
        run_as_admin()
    
    tester = ZaslonTester()
    tester.run_menu()
