import os
import sys
import shutil
import subprocess
import time
import ctypes
import psutil
from threading import Thread, Event

# ZASLON 2.0: Fake System Process Simulator (Enhanced)
# Этот скрипт создает процессы, которые имитируют системные (svchost, lsass и др.),
# но запущены из "подозрительных" папок. Это нужно для проверки функции ZASLON "Fake System Hunter".
# Теперь поддерживает мониторинг в реальном времени.

FAKE_DIR = os.path.join(os.environ['USERPROFILE'], 'Desktop', 'zaslon_fake_sys_test')
SYS_NAMES = ["svchost.exe", "lsass.exe", "csrss.exe", "winlogon.exe", "services.exe", "smss.exe"]

# ANSI colors for Windows console (if supported)
class Colors:
    BLUE = '\033[94m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    CYAN = '\033[96m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    # Aliases for compatibility
    HEADER = '\033[95m'
    OKBLUE = BLUE
    OKCYAN = CYAN
    OKGREEN = GREEN
    WARNING = YELLOW
    FAIL = RED

# Check if terminal supports ANSI
def init_colors():
    if os.name == 'nt':
        kernel32 = ctypes.windll.kernel32
        kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)

def is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

def run_as_admin():
    if is_admin():
        return
    ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, " ".join(sys.argv), None, 1)
    sys.exit()

def setup_fake_sys():
    if not os.path.exists(FAKE_DIR):
        os.makedirs(FAKE_DIR)
    
    source_exe = os.path.join(os.environ['WINDIR'], 'System32', 'notepad.exe')
    
    print(f"[*] Подготовка фейковых процессов в: {FAKE_DIR}")
    for name in SYS_NAMES:
        dest_path = os.path.join(FAKE_DIR, name)
        try:
            shutil.copy2(source_exe, dest_path)
            print(f" [+] Создан: {name}")
        except Exception as e:
            print(f" [-] Ошибка при создании {name}: {e}")

def launch_fake_sys():
    launched = []
    print("\n[*] Запуск фейковых процессов...")
    for name in SYS_NAMES:
        path = os.path.join(FAKE_DIR, name)
        if os.path.exists(path):
            try:
                p = subprocess.Popen([path], creationflags=subprocess.CREATE_NO_WINDOW)
                launched.append(p)
                print(f" [+] Запущен: {name} (PID: {p.pid})")
            except Exception as e:
                print(f" [-] Ошибка при запуске {name}: {e}")
    return launched

def get_active_fake_pids():
    pids = {}
    for proc in psutil.process_iter(['pid', 'name', 'exe']):
        try:
            name = proc.info['name'].lower()
            if any(name == sys_name.lower() for sys_name in SYS_NAMES):
                exe_path = proc.info['exe']
                if exe_path and FAKE_DIR.lower() in exe_path.lower():
                    if name not in pids: pids[name] = []
                    pids[name].append(proc.info['pid'])
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return pids

def real_time_monitor(stop_event):
    print(f"\n{Colors.HEADER}{Colors.BOLD}=== РЕЖИМ МОНИТОРИНГА В РЕАЛЬНОМ ВРЕМЕНИ ==={Colors.ENDC}")
    print(f"Скрипт сообщит, когда ZASLON завершит фейковые процессы.")
    print(f"Нажмите Ctrl+C для выхода из мониторинга.\n")
    
    last_pids = get_active_fake_pids()
    
    try:
        while not stop_event.is_set():
            time.sleep(1)
            current_pids = get_active_fake_pids()
            
            # Проверка завершенных
            for name in last_pids:
                if name not in current_pids:
                    # Весь вид процесса пропал
                    print(f"{Colors.OKGREEN}[IDLE] {name.upper()} полностью завершен! (ZASLON сработал?){Colors.ENDC}")
                else:
                    # Сравним конкретные PIDs
                    killed_pids = set(last_pids[name]) - set(current_pids[name])
                    for pid in killed_pids:
                        print(f"{Colors.OKGREEN}[KILLED] Процесс {name} (PID: {pid}) был ЗАВЕРШЕН!{Colors.ENDC}")
            
            # Проверка новых (если вдруг запустили еще раз)
            for name in current_pids:
                if name not in last_pids:
                    print(f"{Colors.FAIL}[ALERT] Обнаружен НОВЫЙ фейковый процесс: {name} (PIDs: {current_pids[name]}){Colors.ENDC}")
                else:
                    new_pids = set(current_pids[name]) - set(last_pids[name])
                    for pid in new_pids:
                        print(f"{Colors.FAIL}[ALERT] Запущен новый экземпляр {name} (PID: {pid}){Colors.ENDC}")
            
            last_pids = current_pids
            
    except KeyboardInterrupt:
        pass
    print(f"\n{Colors.HEADER}--- Мониторинг остановлен ---{Colors.ENDC}")

def check_status():
    print("\n" + "="*40)
    print("   СТАТУС ФЕЙКОВЫХ ПРОЦЕССОВ")
    print("="*40)
    
    active_pids = get_active_fake_pids()
    found_any = False
    
    for name in SYS_NAMES:
        lower_name = name.lower()
        if lower_name in active_pids:
            pids = active_pids[lower_name]
            status = f"{Colors.FAIL}ПРИСУТСТВУЕТ (PIDs: {', '.join(map(str, pids))}){Colors.ENDC}"
            found_any = True
            mark = "[!]"
        else:
            status = f"{Colors.OKGREEN}ОТСУТСТВУЕТ (Удален?){Colors.ENDC}"
            mark = "[OK]"
        
        print(f"{mark} {name:15}: {status}")
        
    print("="*40)
    if found_any:
        print("!!! ZASLON должен обнаружить и предложить удалить эти процессы !!!")
    else:
        print("Все чисто. Либо вы не запускали тест, либо ZASLON уже всё почистил.")

def cleanup():
    print("\n[*] Очистка...")
    for proc in psutil.process_iter(['pid', 'name', 'exe']):
        try:
            if any(name.lower() == proc.info['name'].lower() for name in SYS_NAMES):
                exe_path = proc.info['exe']
                if exe_path and FAKE_DIR.lower() in exe_path.lower():
                    print(f" [+] Принудительное завершение: {proc.info['name']} (PID: {proc.info['pid']})")
                    proc.terminate()
        except: continue
    
    time.sleep(1)
    if os.path.exists(FAKE_DIR):
        try:
            shutil.rmtree(FAKE_DIR)
            print(" [+] Папка теста удалена.")
        except Exception as e:
            print(f" [-] Ошибка при удалении папки: {e}")

def main_menu():
    init_colors()
    while True:
        # Не очищаем экран в режиме мониторинга, чтобы видеть историю
        print("\n========================================")
        print("   ZASLON: ТЕСТЕР FAKE SYSTEM (v2.1)")
        print("========================================")
        print("1. Подготовить файлы (Setup)")
        print("2. ПОЛНЫЙ ТЕСТ (Запуск + Мониторинг)")
        print("3. Проверить статус (Check)")
        print("5. Очистить всё (Cleanup)")
        print("-" * 40)
        print("0. Выход")
        
        try:
            choice = input("\nВыберите действие: ")
        except EOFError:
            break
            
        if choice == "1":
            setup_fake_sys()
            input("\nГотово. Нажмите Enter...")
        elif choice == "2":
            print(f"{Colors.YELLOW}Запуск процессов и переход в режим мониторинга...{Colors.ENDC}")
            launch_fake_sys()
            
            # Start monitoring automatically after launch
            stop_event = Event()
            print(f"{Colors.GREEN}Мониторинг запущен. Нажмите Ctrl+C для остановки.{Colors.ENDC}")
            try:
                real_time_monitor(stop_event)
            except KeyboardInterrupt:
                stop_event.set()
                print(f"\n{Colors.YELLOW}Мониторинг остановлен.{Colors.ENDC}")
            input("\nНажмите Enter для возврата в меню...")
        elif choice == "3":
            check_status()
            input("\nНажмите Enter...")
        elif choice == "5":
            cleanup()
            input("\nНажмите Enter...")
        elif choice == "0":
            break

if __name__ == "__main__":
    if not is_admin():
        print("Запрос прав администратора...")
        run_as_admin()
        
    main_menu()
