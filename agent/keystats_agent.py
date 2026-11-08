#!/usr/bin/env python3
"""
KeyStats Agent - captura estatisticas do teclado no PC e envia para o painel
ESP32 (Cheap Yellow Display) via USB serial.

Privacidade: o agente guarda e envia APENAS contagens agregadas por tecla.
A sequencia do que foi digitado nunca e armazenada nem transmitida.

Uso:
    python keystats_agent.py                 # detecta a porta automaticamente
    python keystats_agent.py --port COM5     # porta especifica
    python keystats_agent.py --demo          # dados simulados (sem capturar teclado)
    python keystats_agent.py --stdout        # imprime o JSON em vez de enviar
    python keystats_agent.py --layout ansi   # teclado US (padrao: abnt2)
"""

import argparse
import collections
import ctypes
import json
import os
import random
import subprocess
import sys
import threading
import time
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover
    serial = None

# --------------------------------------------------------------------------- config
KPM_WINDOW_S = 10.0     # janela movel do KPM (curta = medidor mais "vivo")
SEND_INTERVAL_S = 0.2   # 5 atualizacoes por segundo
SAVE_INTERVAL_S = 30.0
HEAT_INTERVAL_S = 2.0   # mapa de calor muda devagar; manda com menos frequencia
TOP_N = 8
BAUD = 115200

# VIDs dos conversores USB-serial comuns em placas ESP32
KNOWN_VIDS = {
    0x1A86: "CH340/CH9102",
    0x10C4: "CP210x",
    0x0403: "FTDI",
    0x303A: "Espressif USB",
}

DEFAULT_STATE = Path.home() / ".keystats" / "state.json"


# --------------------------------------------------------------------------- nomes de tecla
SPECIAL_NAMES = {
    "space": "SPACE", "enter": "ENTER", "tab": "TAB", "backspace": "BKSP",
    "esc": "ESC", "caps_lock": "CAPS", "delete": "DEL", "insert": "INS",
    "home": "HOME", "end": "END", "page_up": "PGUP", "page_down": "PGDN",
    "up": "UP", "down": "DOWN", "left": "LEFT", "right": "RIGHT",
    "shift": "SHIFT", "shift_l": "SHIFT", "shift_r": "RSHIFT",
    "ctrl": "CTRL", "ctrl_l": "CTRL", "ctrl_r": "RCTRL",
    "alt": "ALT", "alt_l": "ALT", "alt_r": "ALTGR", "alt_gr": "ALTGR",
    "cmd": "WIN", "cmd_l": "WIN", "cmd_r": "RWIN",
    "menu": "MENU", "print_screen": "PRTSC", "scroll_lock": "SCRLK",
    "pause": "PAUSE", "num_lock": "NUMLK",
}


# Os nomes abaixo precisam bater com firmware/keystats_cyd/keyboard_layout.h.
# Caracteres com Shift sao convertidos para a tecla fisica (ex.: "!" -> "1"),
# assim cada tecla do mapa de calor acumula todas as suas variacoes.
_SHIFTED_COMMON = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "&": "7", "*": "8",
    "(": "9", ")": "0", "_": "-", "+": "=", "<": ",", ">": ".", "{": "[",
    "}": "]", "|": "\\",
}
SHIFTED = {
    # ABNT2: ' " | ´ ` | ~ ^ | ; : | / ? | ç Ç | 6 ¨
    "abnt2": {**_SHIFTED_COMMON, '"': "'", ":": ";", "?": "/", "^": "~",
              "\u02dc": "~", "\u00b4": "ACUTE", "`": "ACUTE", "\u00a8": "6",
              "\u00e7": "CCED", "\u00c7": "CCED",
              # acentos "mortos" chegam como caracteres combinantes em alguns sistemas
              "\u0301": "ACUTE", "\u0300": "ACUTE", "\u0303": "~", "\u0302": "~", "\u0308": "6"},
    # ANSI (US): ` ~ | ' " | ; : | / ?
    "ansi": {**_SHIFTED_COMMON, "~": "`", '"': "'", ":": ";", "?": "/", "^": "6"},
}
# Windows: virtual-key codes das teclas de pontuacao (independem de Shift e acentos)
WIN_OEM_VK = {
    "abnt2": {0xBA: "CCED", 0xBB: "=", 0xBC: ",", 0xBD: "-", 0xBE: ".", 0xBF: ";",
              0xC0: "'", 0xC1: "/", 0xDB: "ACUTE", 0xDC: "]", 0xDD: "[", 0xDE: "~",
              0xE2: "\\"},
    "ansi": {0xBA: ";", 0xBB: "=", 0xBC: ",", 0xBD: "-", 0xBE: ".", 0xBF: "/",
             0xC0: "`", 0xDB: "[", 0xDC: "\\", 0xDD: "]", 0xDE: "'"},
}
# Linux/X11: keysyms das teclas mortas
X11_DEAD = {
    "abnt2": {0xFE51: "ACUTE", 0xFE50: "ACUTE", 0xFE53: "~", 0xFE52: "~", 0xFE57: "6"},
    "ansi": {},
}


def key_name(key, layout="abnt2"):
    """Converte um evento do pynput em um nome curto e estavel (ASCII, max. 8 chars)."""
    name = getattr(key, "name", None)
    if name:  # tecla especial (pynput.keyboard.Key)
        return SPECIAL_NAMES.get(name, name.upper()[:8])

    vk = getattr(key, "vk", None)
    char = getattr(key, "char", None)

    # No Windows o virtual-key code identifica a tecla fisica, independente de Shift
    if sys.platform == "win32" and vk is not None:
        if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:
            return chr(vk)
        if 0x60 <= vk <= 0x69:
            return f"NUM{vk - 0x60}"
        if vk in WIN_OEM_VK[layout]:
            return WIN_OEM_VK[layout][vk]

    if vk is not None and vk in X11_DEAD[layout]:
        return X11_DEAD[layout][vk]

    if char:
        if ord(char) < 32:          # Ctrl+letra gera caracteres de controle
            return chr(ord(char) + 64)
        if char in SHIFTED[layout]:
            return SHIFTED[layout][char]
        up = char.upper()
        if up.isascii():
            return up
        return f"U{ord(char):04X}"  # outros caracteres: nome ASCII estavel
    if vk is not None:
        return f"VK{vk}"
    return "?"


# --------------------------------------------------------------------------- estatisticas
class KeyStats:
    def __init__(self, state_file: Path):
        self.state_file = state_file
        self.lock = threading.Lock()
        self.total = 0
        self.counts = collections.Counter()
        self.session = 0
        self.session_start = time.monotonic()
        self.recent = collections.deque()
        self.peak = 0
        self.pressed = set()
        self._load()

    # ---- persistencia
    def _load(self):
        try:
            data = json.loads(self.state_file.read_text(encoding="utf-8"))
            self.total = int(data.get("total", 0))
            self.counts.update({k: int(v) for k, v in data.get("counts", {}).items()})
        except FileNotFoundError:
            pass
        except (ValueError, OSError) as exc:
            print(f"[aviso] nao consegui ler {self.state_file}: {exc}", file=sys.stderr)

    def save(self):
        with self.lock:
            data = {"total": self.total, "counts": dict(self.counts)}
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.state_file.with_suffix(".tmp")
        tmp.write_text(json.dumps(data), encoding="utf-8")
        os.replace(tmp, self.state_file)

    # ---- eventos
    def press(self, name: str):
        with self.lock:
            if name in self.pressed:   # ignora auto-repeat de tecla segurada
                return
            self.pressed.add(name)
            self.total += 1
            self.session += 1
            self.counts[name] += 1
            self.recent.append(time.monotonic())

    def release(self, name: str):
        with self.lock:
            self.pressed.discard(name)

    # ---- leitura
    def snapshot(self, app: str = "") -> dict:
        now = time.monotonic()
        with self.lock:
            while self.recent and now - self.recent[0] > KPM_WINDOW_S:
                self.recent.popleft()
            kpm = round(len(self.recent) * 60.0 / KPM_WINDOW_S)
            self.peak = max(self.peak, kpm)
            top = [[k, v] for k, v in self.counts.most_common(TOP_N)]
            return {
                "kpm": kpm,
                "peak": self.peak,
                "sess": self.session,
                "total": self.total,
                "up": int(now - self.session_start),
                "app": app[:39],
                "top": top,
            }

    def heat_snapshot(self) -> dict:
        """Intensidade 1..255 por tecla (escala raiz quadrada, para teclas pouco
        usadas continuarem visiveis ao lado de W/A/S/D)."""
        with self.lock:
            items = [(k, v) for k, v in self.counts.most_common(120) if v > 0]
        if not items:
            return {"heat": {}, "hmax": 0}
        hmax = items[0][1]
        heat = {k: max(1, round(255 * (v / hmax) ** 0.5)) for k, v in items}
        return {"heat": heat, "hmax": hmax}


# --------------------------------------------------------------------------- app ativo
def active_app() -> str:
    """Nome do processo da janela em foco (melhor esforco; '' se indisponivel)."""
    try:
        if sys.platform == "win32":
            user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
            hwnd = user32.GetForegroundWindow()
            pid = ctypes.c_ulong()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            h = kernel32.OpenProcess(0x1000, False, pid.value)  # QUERY_LIMITED_INFORMATION
            if not h:
                return ""
            try:
                buf = ctypes.create_unicode_buffer(260)
                size = ctypes.c_ulong(260)
                if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                    return os.path.basename(buf.value)
            finally:
                kernel32.CloseHandle(h)
        elif sys.platform == "darwin":
            out = subprocess.run(
                ["osascript", "-e",
                 'tell application "System Events" to get name of first '
                 'application process whose frontmost is true'],
                capture_output=True, text=True, timeout=1)
            return out.stdout.strip()
        else:
            out = subprocess.run(["xdotool", "getactivewindow", "getwindowpid"],
                                 capture_output=True, text=True, timeout=1)
            pid = out.stdout.strip()
            if pid:
                return Path(f"/proc/{pid}/comm").read_text().strip()
    except Exception:
        pass
    return ""


class AppWatcher(threading.Thread):
    def __init__(self, interval=1.0):
        super().__init__(daemon=True)
        self.interval = interval
        self.current = ""

    def run(self):
        while True:
            self.current = active_app()
            time.sleep(self.interval)


# --------------------------------------------------------------------------- fontes de eventos
def start_keyboard_capture(stats: KeyStats, layout: str):
    try:
        from pynput import keyboard
    except ImportError:
        sys.exit("Instale as dependencias: pip install -r requirements.txt")

    listener = keyboard.Listener(
        on_press=lambda k: stats.press(key_name(k, layout)),
        on_release=lambda k: stats.release(key_name(k, layout)),
    )
    listener.daemon = True
    listener.start()
    return listener


def start_demo(stats: KeyStats):
    """Gera digitacao simulada em rajadas, util para testar o painel."""
    keys = ["W"] * 30 + ["A"] * 18 + ["S"] * 14 + ["D"] * 18 + \
           ["SPACE"] * 10 + ["SHIFT"] * 8 + ["E", "R", "Q", "F", "1", "2", "3", "TAB", "CTRL"] * 2 + \
           list("GZXCVBTY45") + ["ESC", "F1", "ENTER", "CCED", "M", "I", "O", "P", "K", "L"]

    def run():
        while True:
            burst_kpm = random.choice([60, 120, 200, 320, 450])
            end = time.monotonic() + random.uniform(3, 8)
            while time.monotonic() < end:
                k = random.choice(keys)
                stats.press(k)
                stats.release(k)
                time.sleep(60.0 / burst_kpm * random.uniform(0.6, 1.4))
            time.sleep(random.uniform(0.5, 3))

    threading.Thread(target=run, daemon=True).start()


# --------------------------------------------------------------------------- serial
def find_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid in KNOWN_VIDS:
            return p.device, KNOWN_VIDS[p.vid]
    return None, None


def open_serial(port: str):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0
    ser.write_timeout = 1
    # evita resetar o ESP32 ao abrir a porta (DTR/RTS ligados ao EN/BOOT)
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Envia estatisticas de teclado para o KeyStats Panel")
    ap.add_argument("--port", help="porta serial (ex.: COM5, /dev/ttyUSB0). Padrao: auto")
    ap.add_argument("--demo", action="store_true", help="usa digitacao simulada")
    ap.add_argument("--stdout", action="store_true", help="imprime o JSON em vez de enviar pela serial")
    ap.add_argument("--state", type=Path, default=DEFAULT_STATE, help="arquivo de estado (contagens)")
    ap.add_argument("--no-app", action="store_true", help="nao detectar o aplicativo ativo")
    ap.add_argument("--layout", choices=sorted(SHIFTED), default="abnt2",
                    help="layout fisico do teclado (padrao: abnt2)")
    args = ap.parse_args()

    if not args.stdout and serial is None:
        sys.exit("Instale as dependencias: pip install -r requirements.txt")

    # no modo demo nao misturamos dados falsos com o historico real
    state_file = args.state if not args.demo else args.state.with_name("demo_state.json")
    stats = KeyStats(state_file)

    if args.demo:
        start_demo(stats)
        print("Modo demo: gerando digitacao simulada.")
    else:
        start_keyboard_capture(stats, args.layout)
        print("Capturando teclado (apenas contagens). Ctrl+C para sair.")

    watcher = None
    if not args.no_app:
        watcher = AppWatcher()
        watcher.start()

    ser = None
    last_save = time.monotonic()
    last_try = 0.0
    last_heat = 0.0

    try:
        while True:
            msg = stats.snapshot(watcher.current if watcher else "")
            line = json.dumps(msg, separators=(",", ":"), ensure_ascii=True) + "\n"
            if time.monotonic() - last_heat >= HEAT_INTERVAL_S:
                last_heat = time.monotonic()
                line += json.dumps(stats.heat_snapshot(), separators=(",", ":"),
                                   ensure_ascii=True) + "\n"

            if args.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
            else:
                if ser is None and time.monotonic() - last_try > 2.0:
                    last_try = time.monotonic()
                    port, kind = (args.port, "manual") if args.port else find_port()
                    if port:
                        try:
                            ser = open_serial(port)
                            print(f"Conectado em {port} ({kind}).")
                        except serial.SerialException as exc:
                            print(f"[aviso] nao consegui abrir {port}: {exc}")
                    else:
                        print("Painel nao encontrado. Conecte o USB ou use --port.")
                if ser is not None:
                    try:
                        ser.write(line.encode("ascii"))
                        ser.reset_input_buffer()  # descarta logs de boot do ESP32
                    except (serial.SerialException, OSError) as exc:
                        print(f"[aviso] conexao perdida: {exc}")
                        try:
                            ser.close()
                        except Exception:
                            pass
                        ser = None

            if time.monotonic() - last_save > SAVE_INTERVAL_S:
                stats.save()
                last_save = time.monotonic()

            time.sleep(SEND_INTERVAL_S)
    except KeyboardInterrupt:
        print("\nSaindo...")
    finally:
        stats.save()
        if ser is not None:
            ser.close()


if __name__ == "__main__":
    main()
