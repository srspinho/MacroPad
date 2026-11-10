"""
Inicializacao automatica do KeyStats Agent junto com o sistema.

- Windows: valor em HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run, usando
  pythonw.exe (sem janela de terminal). Nao precisa de administrador.
- macOS:   LaunchAgent em ~/Library/LaunchAgents.
- Linux:   arquivo .desktop em ~/.config/autostart (padrao XDG).

O agente sempre inicia com --tray (icone na bandeja, sem terminal).
"""

import os
import plistlib
import shlex
import subprocess
import sys
from pathlib import Path

APP_ID = "KeyStatsAgent"
MAC_LABEL = "com.keystats.agent"
WIN_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
SCRIPT = Path(__file__).resolve().with_name("keystats_agent.py")


def _python_gui() -> str:
    """Interpretador que nao abre janela de terminal (pythonw no Windows)."""
    exe = Path(sys.executable)
    if sys.platform == "win32":
        candidate = exe.with_name("pythonw.exe")
        if candidate.exists():
            return str(candidate)
    return str(exe)


def command(extra_args=()) -> list:
    return [_python_gui(), str(SCRIPT), "--tray", *extra_args]


def _mac_plist() -> Path:
    return Path.home() / "Library" / "LaunchAgents" / f"{MAC_LABEL}.plist"


def _linux_desktop() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "autostart" / "keystats-agent.desktop"


def install(extra_args=()) -> str:
    """Ativa a inicializacao automatica. Retorna onde foi registrada."""
    cmd = command(extra_args)

    if sys.platform == "win32":
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, WIN_RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
            winreg.SetValueEx(k, APP_ID, 0, winreg.REG_SZ, subprocess.list2cmdline(cmd))
        return rf"HKCU\{WIN_RUN_KEY}\{APP_ID}"

    if sys.platform == "darwin":
        path = _mac_plist()
        path.parent.mkdir(parents=True, exist_ok=True)
        log_dir = Path.home() / ".keystats"
        log_dir.mkdir(parents=True, exist_ok=True)
        with open(path, "wb") as f:
            plistlib.dump({
                "Label": MAC_LABEL,
                "ProgramArguments": cmd,
                "RunAtLoad": True,
                "KeepAlive": False,
                "WorkingDirectory": str(SCRIPT.parent),
                "StandardErrorPath": str(log_dir / "launchd.err.log"),
            }, f)
        return str(path)

    path = _linux_desktop()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=KeyStats Agent\n"
        "Comment=Envia estatisticas de teclado para o KeyStats Panel\n"
        f"Exec={shlex.join(cmd)}\n"
        f"Path={SCRIPT.parent}\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n",
        encoding="utf-8",
    )
    return str(path)


def uninstall() -> bool:
    """Remove a inicializacao automatica. Retorna True se havia algo para remover."""
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, WIN_RUN_KEY, 0,
                                winreg.KEY_SET_VALUE) as k:
                winreg.DeleteValue(k, APP_ID)
            return True
        except FileNotFoundError:
            return False

    path = _mac_plist() if sys.platform == "darwin" else _linux_desktop()
    if path.exists():
        path.unlink()
        return True
    return False


def is_installed() -> bool:
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, WIN_RUN_KEY) as k:
                winreg.QueryValueEx(k, APP_ID)
            return True
        except FileNotFoundError:
            return False
    path = _mac_plist() if sys.platform == "darwin" else _linux_desktop()
    return path.exists()
