"""Testes do agente. Rode com:  python -m unittest test_keystats_agent.py"""

import re
import unittest
from pathlib import Path
from types import SimpleNamespace as K

from keystats_agent import KeyStats, key_name

LAYOUT_H = Path(__file__).resolve().parent.parent / "firmware" / "keystats_cyd" / "keyboard_layout.h"


def firmware_names(layout):
    """Extrai do keyboard_layout.h os nomes de tecla de um layout."""
    src = LAYOUT_H.read_text(encoding="utf-8")
    common, rest = src.split("#ifndef KEYSTATS_LAYOUT_ANSI", 1)
    abnt2, rest = rest.split("#else", 1)
    ansi = rest.split("#endif", 1)[0]
    body = common + (abnt2 if layout == "abnt2" else ansi)
    names = re.findall(r'\{"((?:[^"\\]|\\.)+)",', body)
    return {n.encode().decode("unicode_escape") for n in names}


class KeyNameTests(unittest.TestCase):
    def test_letters_digits_special(self):
        self.assertEqual(key_name(K(char="a", vk=97)), "A")
        self.assertEqual(key_name(K(char="\x01", vk=97)), "A")      # Ctrl+A
        self.assertEqual(key_name(K(name="space")), "SPACE")
        self.assertEqual(key_name(K(name="shift_r")), "RSHIFT")
        self.assertEqual(key_name(K(name="f5")), "F5")

    def test_shifted_chars_map_to_physical_key(self):
        self.assertEqual(key_name(K(char="!", vk=None)), "1")
        self.assertEqual(key_name(K(char="?", vk=None), "abnt2"), "/")
        self.assertEqual(key_name(K(char="^", vk=None), "abnt2"), "~")
        self.assertEqual(key_name(K(char="~", vk=None), "ansi"), "`")

    def test_abnt2_specials(self):
        self.assertEqual(key_name(K(char="ç", vk=None), "abnt2"), "CCED")
        self.assertEqual(key_name(K(char="Ç", vk=None), "abnt2"), "CCED")
        self.assertEqual(key_name(K(char="´", vk=None), "abnt2"), "ACUTE")
        self.assertEqual(key_name(K(char=None, vk=0xFE51), "abnt2"), "ACUTE")

    def test_names_are_ascii(self):
        self.assertTrue(key_name(K(char="é", vk=None)).isascii())

    def test_agent_names_exist_in_firmware_layout(self):
        typable = {
            "abnt2": "abcdefghijklmnopqrstuvwxyz0123456789'\"!@#$%&*()-_=+[{]};:,<.>/?\\|~^´`çÇ",
            "ansi": "abcdefghijklmnopqrstuvwxyz0123456789`~!@#$%^&*()-_=+[{]}\\|;:'\",<.>/?",
        }
        specials = ["esc", "tab", "caps_lock", "shift", "shift_r", "ctrl_l", "ctrl_r",
                    "alt_l", "alt_gr", "cmd", "space", "enter", "backspace", "delete",
                    "home", "end", "page_up", "page_down", "up", "down", "left", "right",
                    *[f"f{i}" for i in range(1, 13)]]
        for layout, chars in typable.items():
            fw = firmware_names(layout)
            for c in chars:
                self.assertIn(key_name(K(char=c, vk=None), layout), fw, f"{layout}: {c!r}")
            for sp in specials:
                self.assertIn(key_name(K(name=sp), layout), fw, f"{layout}: {sp}")


class HeatTests(unittest.TestCase):
    def test_heat_scale(self):
        st = KeyStats(Path("/nonexistent/state.json"))
        for _ in range(100):
            st.press("W"); st.release("W")
        st.press("Q"); st.release("Q")
        h = st.heat_snapshot()
        self.assertEqual(h["hmax"], 100)
        self.assertEqual(h["heat"]["W"], 255)
        self.assertEqual(h["heat"]["Q"], 26)   # sqrt(1/100) * 255

    def test_held_key_counts_once(self):
        st = KeyStats(Path("/nonexistent/state.json"))
        for _ in range(5):
            st.press("W")                       # auto-repeat
        st.release("W")
        self.assertEqual(st.total, 1)


if __name__ == "__main__":
    unittest.main()
