import tkinter as tk
import ctypes
import os
import sys

# ZASLON 2.0 Test WinLocker
# This script simulates a screen locker to test "Anti-WinLocker" (Isolated Desktop) feature.
# It does NOT encrypt files. It only blocks the UI.

def start_locker():
    root = tk.Tk()
    root.title("ZASLON TEST WINLOCKER")
    
    # Hide window decorations and make fullscreen
    root.overrideredirect(True)
    width = root.winfo_screenwidth()
    height = root.winfo_screenheight()
    root.geometry(f"{width}x{height}+0+0")
    
    root.configure(bg='black')
    
    # Text label
    label = tk.Label(root, text="[!] ВНИМАНИЕ [!]\n\nВаша система заблокирована симуляцией сильного винлокера.\nЭто ТЕСТ для функции ZASLON: Anti-WinLocker.\n\nПопробуйте запустить ZASLON и нажать 'Anti-WinLocker' (Alt Desktop).\n\nЧтобы ВЫЙТИ без ZASLON, нажмите: Ctrl + Shift + X", 
                    fg="red", bg="black", font=("Arial", 24, "bold"), justify="center")
    label.pack(expand=True)

    # Force TopMost using WinAPI
    try:
        HWND_TOPMOST = -1
        SWP_NOMOVE = 0x0002
        SWP_NOSIZE = 0x0001
        SWP_SHOWWINDOW = 0x0040
        
        def make_topmost():
            hwnd = ctypes.windll.user32.GetParent(root.winfo_id())
            ctypes.windll.user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)
            root.focus_force()
            root.after(100, make_topmost)
        
        make_topmost()
    except Exception as e:
        print(f"Error setting topmost: {e}")

    # Trap Ctrl+Shift+X for exit
    def safe_exit(event=None):
        root.destroy()
        sys.exit(0)

    root.bind("<Control-Shift-Key-X>", safe_exit)
    root.bind("<Control-Shift-Key-x>", safe_exit)

    # Disable interaction with background
    root.grab_set()
    
    root.mainloop()

if __name__ == "__main__":
    start_locker()
