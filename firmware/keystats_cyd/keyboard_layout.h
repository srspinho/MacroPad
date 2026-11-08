// Layouts de teclado para a tela de mapa de calor.
//
// Formato 75% (16 unidades de largura) para caber nos 320 px do display.
// Larguras em "quartos de tecla" (4 = 1u). name == nullptr indica um espaco vazio.
// Os nomes PRECISAM bater com os gerados por keystats_agent.py (key_name()).
//
// Padrao: ABNT2. Para ANSI (US), descomente KEYSTATS_LAYOUT_ANSI
// e rode o agente com --layout ansi.

#pragma once
#include <stdint.h>

// #define KEYSTATS_LAYOUT_ANSI

struct KeySpec {
  const char *name;   // nome enviado pelo agente
  const char *label;  // texto no keycap (nullptr = usa o nome)
  uint8_t     w;      // largura em quartos de tecla
};

#define GAP(w) {nullptr, nullptr, w}

static const KeySpec ROW_F[] = {
  {"ESC", "Esc", 4}, GAP(4),
  {"F1", nullptr, 4}, {"F2", nullptr, 4}, {"F3", nullptr, 4}, {"F4", nullptr, 4}, GAP(2),
  {"F5", nullptr, 4}, {"F6", nullptr, 4}, {"F7", nullptr, 4}, {"F8", nullptr, 4}, GAP(2),
  {"F9", nullptr, 4}, {"F10", nullptr, 4}, {"F11", nullptr, 4}, {"F12", nullptr, 4},
  {"DEL", "Del", 4},
};

static const KeySpec ROW_BOTTOM[] = {
  {"CTRL", "Ctrl", 5}, {"WIN", "Win", 5}, {"ALT", "Alt", 5},
  {"SPACE", "Space", 25}, {"ALTGR", "AGr", 5}, {"RCTRL", "Ctrl", 5}, GAP(2),
  {"LEFT", "<", 4}, {"DOWN", "v", 4}, {"RIGHT", ">", 4},
};

#ifndef KEYSTATS_LAYOUT_ANSI
// ------------------------------------------------------------------ ABNT2
#define LAYOUT_NAME "ABNT2"
static const KeySpec ROW_NUM[] = {
  {"'", nullptr, 4}, {"1", nullptr, 4}, {"2", nullptr, 4}, {"3", nullptr, 4}, {"4", nullptr, 4},
  {"5", nullptr, 4}, {"6", nullptr, 4}, {"7", nullptr, 4}, {"8", nullptr, 4}, {"9", nullptr, 4},
  {"0", nullptr, 4}, {"-", nullptr, 4}, {"=", nullptr, 4}, {"BKSP", "Bksp", 8}, {"HOME", "Hom", 4},
};
static const KeySpec ROW_TOP[] = {
  {"TAB", "Tab", 6},
  {"Q", nullptr, 4}, {"W", nullptr, 4}, {"E", nullptr, 4}, {"R", nullptr, 4}, {"T", nullptr, 4},
  {"Y", nullptr, 4}, {"U", nullptr, 4}, {"I", nullptr, 4}, {"O", nullptr, 4}, {"P", nullptr, 4},
  {"ACUTE", "ACUTE", 4}, {"[", nullptr, 4}, {"ENTER", "Ent", 6}, {"PGUP", "PgU", 4},
};
static const KeySpec ROW_HOME[] = {
  {"CAPS", "Caps", 7},
  {"A", nullptr, 4}, {"S", nullptr, 4}, {"D", nullptr, 4}, {"F", nullptr, 4}, {"G", nullptr, 4},
  {"H", nullptr, 4}, {"J", nullptr, 4}, {"K", nullptr, 4}, {"L", nullptr, 4},
  {"CCED", "CCED", 4}, {"~", nullptr, 4}, {"]", nullptr, 4}, {"ENTER", "", 5}, {"PGDN", "PgD", 4},
};
static const KeySpec ROW_SHIFT[] = {
  {"SHIFT", "Shf", 4}, {"\\", nullptr, 4},
  {"Z", nullptr, 4}, {"X", nullptr, 4}, {"C", nullptr, 4}, {"V", nullptr, 4}, {"B", nullptr, 4},
  {"N", nullptr, 4}, {"M", nullptr, 4}, {",", nullptr, 4}, {".", nullptr, 4}, {";", nullptr, 4},
  {"/", nullptr, 4}, {"RSHIFT", "Shf", 4}, {"UP", "^", 4}, {"END", "End", 4},
};
#else
// ------------------------------------------------------------------ ANSI (US)
#define LAYOUT_NAME "ANSI"
static const KeySpec ROW_NUM[] = {
  {"`", nullptr, 4}, {"1", nullptr, 4}, {"2", nullptr, 4}, {"3", nullptr, 4}, {"4", nullptr, 4},
  {"5", nullptr, 4}, {"6", nullptr, 4}, {"7", nullptr, 4}, {"8", nullptr, 4}, {"9", nullptr, 4},
  {"0", nullptr, 4}, {"-", nullptr, 4}, {"=", nullptr, 4}, {"BKSP", "Bksp", 8}, {"HOME", "Hom", 4},
};
static const KeySpec ROW_TOP[] = {
  {"TAB", "Tab", 6},
  {"Q", nullptr, 4}, {"W", nullptr, 4}, {"E", nullptr, 4}, {"R", nullptr, 4}, {"T", nullptr, 4},
  {"Y", nullptr, 4}, {"U", nullptr, 4}, {"I", nullptr, 4}, {"O", nullptr, 4}, {"P", nullptr, 4},
  {"[", nullptr, 4}, {"]", nullptr, 4}, {"\\", nullptr, 6}, {"PGUP", "PgU", 4},
};
static const KeySpec ROW_HOME[] = {
  {"CAPS", "Caps", 7},
  {"A", nullptr, 4}, {"S", nullptr, 4}, {"D", nullptr, 4}, {"F", nullptr, 4}, {"G", nullptr, 4},
  {"H", nullptr, 4}, {"J", nullptr, 4}, {"K", nullptr, 4}, {"L", nullptr, 4},
  {";", nullptr, 4}, {"'", nullptr, 4}, {"ENTER", "Enter", 9}, {"PGDN", "PgD", 4},
};
static const KeySpec ROW_SHIFT[] = {
  {"SHIFT", "Shift", 9},
  {"Z", nullptr, 4}, {"X", nullptr, 4}, {"C", nullptr, 4}, {"V", nullptr, 4}, {"B", nullptr, 4},
  {"N", nullptr, 4}, {"M", nullptr, 4}, {",", nullptr, 4}, {".", nullptr, 4}, {"/", nullptr, 4},
  {"RSHIFT", "Shift", 7}, {"UP", "^", 4}, {"END", "End", 4},
};
#endif

#undef GAP

struct LayoutRow {
  const KeySpec *keys;
  uint8_t        count;
  int16_t        y;  // posicao vertical na tela
  uint8_t        h;  // altura das teclas
};

#define ROW(arr, y, h) {arr, (uint8_t)(sizeof(arr) / sizeof(arr[0])), y, h}
static const LayoutRow LAYOUT_ROWS[] = {
  ROW(ROW_F,      30, 20),
  ROW(ROW_NUM,    54, 24),
  ROW(ROW_TOP,    80, 24),
  ROW(ROW_HOME,  106, 24),
  ROW(ROW_SHIFT, 132, 24),
  ROW(ROW_BOTTOM,158, 24),
};
#undef ROW
static const uint8_t LAYOUT_ROW_COUNT = sizeof(LAYOUT_ROWS) / sizeof(LAYOUT_ROWS[0]);
static const uint8_t QUARTER_PX = 5;  // 64 quartos x 5 px = 320 px
