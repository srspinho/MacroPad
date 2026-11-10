"""
Icone na bandeja do sistema para o KeyStats Agent (modo --tray).

Mostra se o painel esta conectado (verde) ou nao (cinza), o KPM atual no
tooltip e um menu com: status, abrir pasta de dados, iniciar com o sistema e sair.
Requer: pip install pystray Pillow
"""

import logging
import os
import subprocess
import sys
import threading

log = logging.getLogger("keystats")

C_CONNECTED = (0, 200, 120)
C_DISCONNECTED = (120, 120, 120)


def make_icon(connected: bool):
    """Desenha um keycap 64x64 com um ponto de status."""
    from PIL import Image, ImageDraw

    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2, 6, 61, 61], radius=12, fill=(20, 24, 40))          # base do keycap
    d.rounded_rectangle([8, 4, 55, 47], radius=9, fill=(0, 190, 220))          # topo
    d.rectangle([18, 16, 23, 37], fill=(255, 255, 255))                        # letra "K"
    d.polygon([(24, 27), (36, 15), (43, 15), (29, 28)], fill=(255, 255, 255))
    d.polygon([(26, 26), (43, 37), (36, 37), (24, 29)], fill=(255, 255, 255))
    color = C_CONNECTED if connected else C_DISCONNECTED
    d.ellipse([40, 40, 62, 62], fill=color, outline=(20, 24, 40), width=3)     # status
    return img


def status_text(status: dict) -> str:
    port = status.get("port")
    if port:
        return f"Conectado em {port} - {status.get('kpm', 0)} KPM"
    return "Painel desconectado"


def open_folder(path):
    path = str(path)
    if sys.platform == "win32":
        os.startfile(path)  # noqa: S606 - abre a pasta no Explorer
    elif sys.platform == "darwin":
        subprocess.Popen(["open", path])
    else:
        subprocess.Popen(["xdg-open", path])


def run_tray(stop: threading.Event, status: dict, worker, autostart_args, data_dir):
    """Roda o icone na thread principal e o `worker` (captura/envio) em segundo plano."""
    try:
        import pystray
    except ImportError:
        log.error("Modo --tray precisa de: pip install pystray Pillow")
        sys.exit(1)

    import autostart

    def work():
        try:
            worker()
        except Exception:
            log.exception("erro no agente")
        finally:
            stop.set()

    thread = threading.Thread(target=work, daemon=True)
    thread.start()

    def on_quit(icon, _item):
        stop.set()
        icon.stop()

    def on_toggle_autostart(_icon, _item):
        try:
            if autostart.is_installed():
                autostart.uninstall()
                log.info("Inicializacao automatica removida.")
            else:
                autostart.install(autostart_args)
                log.info("Inicializacao automatica ativada.")
        except Exception:
            log.exception("nao consegui alterar a inicializacao automatica")

    menu = pystray.Menu(
        pystray.MenuItem(lambda _item: status_text(status), None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Abrir pasta de dados", lambda _i, _it: open_folder(data_dir)),
        pystray.MenuItem("Iniciar com o sistema", on_toggle_autostart,
                         checked=lambda _item: autostart.is_installed()),
        pystray.MenuItem("Sair", on_quit),
    )

    icon = pystray.Icon("keystats", make_icon(False), "KeyStats", menu)

    def updater(icon):
        icon.visible = True
        connected = None
        while not stop.is_set():
            now_connected = bool(status.get("port"))
            if now_connected != connected:
                connected = now_connected
                icon.icon = make_icon(connected)
            icon.title = f"KeyStats - {status_text(status)}"
            stop.wait(1.0)
        icon.stop()  # o worker terminou (erro ou Sair)

    log.info("Rodando na bandeja do sistema.")
    icon.run(setup=updater)
    stop.set()
    thread.join(timeout=5)
