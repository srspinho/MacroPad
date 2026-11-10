/*
  KeyStats Panel - firmware MVP para o "Cheap Yellow Display" (ESP32-2432S028R)

  Recebe estatisticas de teclado do agente no PC (keystats_agent.py) via USB serial,
  uma linha JSON por mensagem, e mostra em quatro telas:
    1) AO VIVO    - KPM atual, pico, teclas da sessao, total e grafico dos ultimos 60 s
    2) POR JOGO   - estatisticas do jogo/app em foco: teclas, recorde de KPM, tempo, top 5
    3) TOP TECLAS - as 8 teclas mais usadas
    4) MAPA DE CALOR - o teclado colorido pela frequencia de uso (ABNT2 ou ANSI),
                       em 3 modos: historico, sessao e jogo atual

  Toque rapido na tela (ou clique no BOOT): proxima tela.
  Segure o dedo na tela por 1 s (ou o BOOT por 1-3 s) no mapa de calor: troca o modo.

  CONFIGURACAO DO DISPLAY: existem varias revisoes do CYD com displays diferentes.
  No primeiro boot (ou segurando BOOT por 3 s) abre um assistente com uma tela de teste:
    - aperte BOOT          -> testa o proximo perfil de display (a placa reinicia)
    - toque na tela        -> gira / espelha a imagem
    - segure BOOT por 2 s  -> salva e comeca a usar o painel
  A escolha fica gravada na flash e sobrevive a novas gravacoes do firmware.

  Bibliotecas (Gerenciador de Bibliotecas da IDE):
    - LovyanGFX   (testado com 1.2.0)
    - ArduinoJson (testado com 7.2.1)
  Placa: "ESP32 Dev Module" (core esp32 da Espressif 3.x)

  Protocolo (115200 baud, uma linha JSON terminada em '\n'):
    {"kpm":312,"peak":540,"sess":1834,"total":184233,"up":3605,
     "app":"cs2.exe","top":[["W",5123],["A",3301],["SPACE",2980]],
     "game":{"name":"cs2.exe","total":9120,"sess":812,"peak":610,"time":7260,
             "top":[["W",2100],["A",1500]]}}
  e linhas com o mapa de calor (intensidade 1..255 por tecla), uma por modo:
    {"heat":{"W":255,"A":204,"SPACE":180},"hmax":5123,"scope":"total"}
    scope: "total" (historico), "sess" (sessao) ou "game" (+ "name": app em foco)
*/

#include "lgfx_cyd.h"
#include "keyboard_layout.h"
#include <ArduinoJson.h>
#include <Preferences.h>

// ---------------------------------------------------------------- configuracao
static const uint32_t SERIAL_BAUD     = 115200;
static const uint32_t LINK_TIMEOUT_MS = 3000;   // sem dados por esse tempo = desconectado
static const uint8_t  TOP_N           = 8;
static const uint8_t  GAME_TOP_N      = 5;
static const uint32_t LONG_PRESS_MS   = 1000;   // toque longo
static const uint32_t TOUCH_UP_MS     = 80;     // tempo sem toque para considerar "soltou"
static const uint8_t  HIST_N          = 60;     // pontos do grafico (1 por segundo)

static const int PIN_BOOT  = 0;                 // botao BOOT
static const int PIN_LED_R = 4;                 // LED RGB do CYD (ativo em LOW)
static const int PIN_LED_G = 16;
static const int PIN_LED_B = 17;

// ---------------------------------------------------------------- cores
static const uint16_t C_BG     = 0x0841;  // quase preto
static const uint16_t C_PANEL  = 0x1082;
static const uint16_t C_GRID   = 0x2124;
static const uint16_t C_TEXT   = 0xFFFF;
static const uint16_t C_DIM    = 0x8410;
static const uint16_t C_ACCENT = 0x07FF;  // ciano
static const uint16_t C_ACC2   = 0xF81F;  // magenta
static const uint16_t C_OK     = 0x07E0;
static const uint16_t C_BAD    = 0xF800;

// ---------------------------------------------------------------- estado
struct Stats {
  uint32_t kpm = 0, peak = 0, sess = 0, total = 0, up = 0;
  char     app[40] = "";
  uint8_t  topCount = 0;
  char     topKey[TOP_N][12];
  uint32_t topVal[TOP_N];

  // jogo/app em foco
  char     gName[40] = "";
  uint32_t gTotal = 0, gSess = 0, gPeak = 0, gTime = 0;
  uint8_t  gTopCount = 0;
  char     gTopKey[GAME_TOP_N][12];
  uint32_t gTopVal[GAME_TOP_N];
};

static LGFX        lcd;
static Preferences prefs;
static LGFX_Sprite sprKpm(&lcd);
static LGFX_Sprite sprGraph(&lcd);
static LGFX_Sprite sprRow(&lcd);
static LGFX_Sprite sprHeader(&lcd);

static Stats    st;
static bool     everConnected = false;
static bool     linkUp        = false;
static uint32_t lastMsgMs     = 0;
static bool     dirty         = true;   // dados novos para desenhar
static bool     topDirty      = true;

static uint16_t hist[HIST_N];
static uint8_t  histHead = 0;           // proxima posicao a escrever

enum Screen : uint8_t { SCR_LIVE = 0, SCR_GAME, SCR_TOP, SCR_HEAT, SCR_COUNT };
static Screen screen = SCR_LIVE;

static char    lineBuf[2048];

// mapa de calor: retangulos calculados a partir do layout + intensidade por tecla
struct KeyRect {
  const char *name;
  const char *label;
  int16_t     x, w;
  uint8_t     row;
};
static const uint8_t MAX_KEYS = 100;
static KeyRect  keys[MAX_KEYS];
static uint8_t  keyCount  = 0;
enum HeatMode : uint8_t { HEAT_TOTAL = 0, HEAT_SESS, HEAT_GAME, HEAT_MODES };
static const char *const HEAT_SCOPE_NAMES[HEAT_MODES] = {"total", "sess", "game"};
static const char *const HEAT_MODE_LABELS[HEAT_MODES] = {"HISTORICO", "SESSAO", "JOGO"};
static uint8_t  heat[HEAT_MODES][MAX_KEYS];
static uint32_t heatMax[HEAT_MODES];
static char     heatGame[40] = "";      // app a que o mapa "game" se refere
static uint8_t  heatMode  = HEAT_TOTAL;
static bool     heatDirty = true;
static bool     gameTitleDirty = true;
static size_t  lineLen = 0;

// ---------------------------------------------------------------- utilidades
static void setLed(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}

static void fmtThousands(uint32_t v, char *out, size_t n) {
  char raw[16];
  snprintf(raw, sizeof(raw), "%lu", (unsigned long)v);
  size_t len = strlen(raw), o = 0;
  for (size_t i = 0; i < len && o + 1 < n; i++) {
    out[o++] = raw[i];
    size_t rest = len - i - 1;
    if (rest > 0 && rest % 3 == 0 && o + 1 < n) out[o++] = '.';
  }
  out[o] = 0;
}

static void fmtDuration(uint32_t s, char *out, size_t n) {
  snprintf(out, n, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600),
           (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
}

// cor do KPM: verde -> amarelo -> vermelho conforme a intensidade
static uint16_t kpmColor(uint32_t kpm) {
  if (kpm < 150) return C_OK;
  if (kpm < 300) return 0xFFE0;  // amarelo
  if (kpm < 450) return 0xFD20;  // laranja
  return C_BAD;
}

// ---------------------------------------------------------------- protocolo
static void handleLine(const char *line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;  // linha invalida: ignora

  JsonObject heatObj = doc["heat"];
  if (!heatObj.isNull()) {
    const char *scope = doc["scope"] | "total";
    uint8_t m = HEAT_TOTAL;
    for (uint8_t i = 0; i < HEAT_MODES; i++)
      if (strcmp(scope, HEAT_SCOPE_NAMES[i]) == 0) m = i;

    memset(heat[m], 0, sizeof(heat[m]));
    for (JsonPair kv : heatObj) {
      const char *k = kv.key().c_str();
      uint8_t v = kv.value() | 0;
      for (uint8_t i = 0; i < keyCount; i++)   // sem break: ENTER ISO ocupa 2 retangulos
        if (strcmp(keys[i].name, k) == 0) heat[m][i] = v;
    }
    heatMax[m] = doc["hmax"] | 0;
    if (m == HEAT_GAME) strlcpy(heatGame, doc["name"] | "", sizeof(heatGame));
    if (m == heatMode) heatDirty = true;
    lastMsgMs = millis();
    everConnected = true;
    return;
  }

  st.kpm   = doc["kpm"]   | 0;
  st.peak  = doc["peak"]  | 0;
  st.sess  = doc["sess"]  | 0;
  st.total = doc["total"] | 0;
  st.up    = doc["up"]    | 0;
  strlcpy(st.app, doc["app"] | "", sizeof(st.app));

  JsonArray top = doc["top"];
  if (!top.isNull()) {
    uint8_t n = 0;
    for (JsonArray item : top) {
      if (n >= TOP_N) break;
      strlcpy(st.topKey[n], item[0] | "?", sizeof(st.topKey[n]));
      st.topVal[n] = item[1] | 0;
      n++;
    }
    st.topCount = n;
    topDirty = true;
  }

  JsonObject game = doc["game"];
  if (!game.isNull()) {
    const char *name = game["name"] | "";
    if (strcmp(name, st.gName) != 0) gameTitleDirty = true;
    strlcpy(st.gName, name, sizeof(st.gName));
    st.gTotal = game["total"] | 0;
    st.gSess  = game["sess"]  | 0;
    st.gPeak  = game["peak"]  | 0;
    st.gTime  = game["time"]  | 0;
    uint8_t n = 0;
    for (JsonArray item : game["top"].as<JsonArray>()) {
      if (n >= GAME_TOP_N) break;
      strlcpy(st.gTopKey[n], item[0] | "?", sizeof(st.gTopKey[n]));
      st.gTopVal[n] = item[1] | 0;
      n++;
    }
    st.gTopCount = n;
  }

  lastMsgMs = millis();
  everConnected = true;
  dirty = true;
}

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = 0;
      if (lineLen > 0) handleLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;  // linha grande demais: descarta
    }
  }
}

// ---------------------------------------------------------------- desenho comum
static void drawHeader() {
  LovyanGFX *g = sprHeader.getBuffer() ? (LovyanGFX *)&sprHeader : (LovyanGFX *)&lcd;

  g->fillRect(0, 0, 320, 24, C_PANEL);
  g->setFont(&fonts::Font2);
  g->setTextDatum(middle_left);
  g->setTextColor(C_ACCENT, C_PANEL);
  g->drawString("KEYSTATS", 6, 12);

  // indicador de conexao
  g->fillCircle(84, 12, 4, linkUp ? C_OK : C_BAD);

  // aplicativo ativo
  g->setTextColor(C_TEXT, C_PANEL);
  g->setClipRect(94, 0, 150, 24);
  g->drawString(linkUp ? (st.app[0] ? st.app : "-") : "Aguardando PC...", 94, 12);
  g->clearClipRect();

  // tempo de sessao
  char buf[16];
  fmtDuration(st.up, buf, sizeof(buf));
  g->setTextDatum(middle_right);
  g->setTextColor(C_DIM, C_PANEL);
  g->drawString(buf, 314, 12);

  if (g == &sprHeader) sprHeader.pushSprite(0, 0);
}

static void drawWaiting() {
  lcd.fillRect(0, 24, 320, 216, C_BG);
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(C_ACCENT, C_BG);
  lcd.drawString("KeyStats Panel", 160, 100);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(C_DIM, C_BG);
  lcd.drawString("Conecte o USB e rode o agente no PC:", 160, 140);
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.drawString("python keystats_agent.py", 160, 162);
}

// ---------------------------------------------------------------- tela AO VIVO
static const int KPM_X = 4, KPM_Y = 28, KPM_W = 192, KPM_H = 104;
static const int BOX_X = 200, BOX_W = 116, BOX_H = 33;
static const int GR_X = 4, GR_Y = 138, GR_W = 312, GR_H = 98;

static void drawStatBoxAt(int x, int y, int w, const char *label, const char *value,
                          uint16_t color) {
  lcd.fillRoundRect(x, y, w, BOX_H, 4, C_PANEL);
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(C_DIM, C_PANEL);
  lcd.drawString(label, x + 6, y + 3);
  lcd.setFont(&fonts::Font2);
  lcd.setTextDatum(bottom_right);
  lcd.setTextColor(color, C_PANEL);
  lcd.drawString(value, x + w - 6, y + BOX_H - 2);
}

static void drawStatBox(int y, const char *label, uint32_t value, uint16_t color) {
  char buf[16];
  fmtThousands(value, buf, sizeof(buf));
  drawStatBoxAt(BOX_X, y, BOX_W, label, buf, color);
}

static void drawKpm() {
  LovyanGFX *g = sprKpm.getBuffer() ? (LovyanGFX *)&sprKpm : (LovyanGFX *)&lcd;
  int ox = (g == &lcd) ? KPM_X : 0, oy = (g == &lcd) ? KPM_Y : 0;

  g->fillRoundRect(ox, oy, KPM_W, KPM_H, 6, C_PANEL);
  char buf[8];
  uint32_t v = st.kpm > 9999 ? 9999 : st.kpm;
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);

  g->setFont(&fonts::Font7);                 // digitos de 7 segmentos
  g->setTextSize(strlen(buf) >= 4 ? 1.15f : 1.5f);
  g->setTextDatum(middle_center);
  g->setTextColor(kpmColor(st.kpm), C_PANEL);
  g->drawString(buf, ox + KPM_W / 2, oy + KPM_H / 2 - 6);
  g->setTextSize(1);

  g->setFont(&fonts::Font2);
  g->setTextDatum(bottom_center);
  g->setTextColor(C_DIM, C_PANEL);
  g->drawString("teclas / minuto", ox + KPM_W / 2, oy + KPM_H - 3);

  if (g == &sprKpm) sprKpm.pushSprite(KPM_X, KPM_Y);
}

static void drawGraph() {
  LovyanGFX *g = sprGraph.getBuffer() ? (LovyanGFX *)&sprGraph : (LovyanGFX *)&lcd;
  int ox = (g == &lcd) ? GR_X : 0, oy = (g == &lcd) ? GR_Y : 0;

  g->fillRoundRect(ox, oy, GR_W, GR_H, 6, C_PANEL);

  uint16_t maxV = 60;
  for (uint8_t i = 0; i < HIST_N; i++) if (hist[i] > maxV) maxV = hist[i];
  maxV = ((maxV + 49) / 50) * 50;  // arredonda a escala para multiplos de 50

  const int px = ox + 4, py = oy + 14, pw = GR_W - 8, ph = GR_H - 18;
  for (int k = 1; k <= 3; k++) g->drawFastHLine(px, py + ph * k / 4, pw, C_GRID);

  g->setFont(&fonts::Font0);
  g->setTextDatum(top_left);
  g->setTextColor(C_DIM, C_PANEL);
  g->drawString("ultimos 60 s", ox + 6, oy + 3);
  char buf[16];
  snprintf(buf, sizeof(buf), "max %u", maxV);
  g->setTextDatum(top_right);
  g->drawString(buf, ox + GR_W - 6, oy + 3);

  // area preenchida + linha
  int prevX = -1, prevY = -1;
  for (uint8_t i = 0; i < HIST_N; i++) {
    uint16_t v = hist[(histHead + i) % HIST_N];   // do mais antigo ao mais novo
    int x = px + (int32_t)i * (pw - 1) / (HIST_N - 1);
    int y = py + ph - (int32_t)v * ph / maxV;
    if (prevX >= 0) {
      for (int xx = prevX; xx <= x; xx++) {
        int yy = prevY + (y - prevY) * (xx - prevX) / (x - prevX > 0 ? x - prevX : 1);
        g->drawFastVLine(xx, yy, py + ph - yy, 0x0210);  // ciano escuro
      }
      g->drawLine(prevX, prevY, x, y, C_ACCENT);
      g->drawLine(prevX, prevY - 1, x, y - 1, C_ACCENT);
    }
    prevX = x; prevY = y;
  }

  if (g == &sprGraph) sprGraph.pushSprite(GR_X, GR_Y);
}

static void drawLiveDynamic() {
  drawKpm();
  drawStatBox(KPM_Y,                     "PICO (KPM)", st.peak,  C_ACC2);
  drawStatBox(KPM_Y + (BOX_H + 2),       "SESSAO",     st.sess,  C_TEXT);
  drawStatBox(KPM_Y + (BOX_H + 2) * 2,   "TOTAL",      st.total, C_ACCENT);
}

// ---------------------------------------------------------------- keycaps
static void buildLayout() {
  keyCount = 0;
  for (uint8_t r = 0; r < LAYOUT_ROW_COUNT; r++) {
    int16_t q = 0;
    for (uint8_t i = 0; i < LAYOUT_ROWS[r].count; i++) {
      const KeySpec &k = LAYOUT_ROWS[r].keys[i];
      if (k.name && keyCount < MAX_KEYS) {
        keys[keyCount++] = {k.name, k.label ? k.label : k.name,
                            (int16_t)(q * QUARTER_PX), (int16_t)(k.w * QUARTER_PX), r};
      }
      q += k.w;
    }
  }
}

// rotulo amigavel de uma tecla (ex.: "BKSP" -> "Bksp"); nomes fora do layout ficam como estao
static const char *labelFor(const char *name) {
  for (uint8_t i = 0; i < keyCount; i++)
    if (strcmp(keys[i].name, name) == 0 && keys[i].label[0]) return keys[i].label;
  return name;
}

// desenha o texto de um keycap; trata as teclas que a fonte nao tem (Ç e acento agudo)
static void drawKeyLabel(LovyanGFX *g, const char *label, int cx, int cy,
                         uint16_t fg, uint16_t bg, bool big) {
  g->setTextDatum(middle_center);
  g->setTextColor(fg, bg);
  if (strcmp(label, "CCED") == 0) {
    g->setFont(&fonts::Font2);
    g->drawString("C", cx, cy - 1);
    g->drawFastVLine(cx, cy + 6, 2, fg);         // cedilha
    g->drawFastHLine(cx - 1, cy + 8, 2, fg);
    return;
  }
  if (strcmp(label, "ACUTE") == 0) {
    g->drawLine(cx - 1, cy + 2, cx + 2, cy - 3, fg);  // acento agudo
    g->drawLine(cx,     cy + 2, cx + 3, cy - 3, fg);
    return;
  }
  if (big || strlen(label) == 1) g->setFont(&fonts::Font2);
  else                           g->setFont(&fonts::Font0);
  g->drawString(label, cx, cy);
}

// ---------------------------------------------------------------- tela TOP TECLAS
static const int ROW_Y0 = 30, ROW_H = 26;

static void drawTopRows() {
  uint32_t maxV = st.topCount ? st.topVal[0] : 1;
  if (maxV == 0) maxV = 1;

  for (uint8_t i = 0; i < TOP_N; i++) {
    LovyanGFX *g = sprRow.getBuffer() ? (LovyanGFX *)&sprRow : (LovyanGFX *)&lcd;
    int oy = (g == &lcd) ? ROW_Y0 + i * ROW_H : 0;

    g->fillRect(0, oy, 320, ROW_H, C_BG);
    if (i < st.topCount) {
      // caixa da tecla (estilo keycap)
      g->fillRoundRect(4, oy + 2, 64, ROW_H - 4, 4, C_PANEL);
      g->drawRoundRect(4, oy + 2, 64, ROW_H - 4, 4, C_GRID);
      drawKeyLabel(g, labelFor(st.topKey[i]), 36, oy + ROW_H / 2, C_TEXT, C_PANEL, true);

      // barra proporcional
      const int bx = 74, bw = 170;
      int w = (int)((uint64_t)st.topVal[i] * bw / maxV);
      if (w < 2) w = 2;
      g->fillRoundRect(bx, oy + 6, bw, ROW_H - 12, 3, C_PANEL);
      g->fillRoundRect(bx, oy + 6, w,  ROW_H - 12, 3, i == 0 ? C_ACC2 : C_ACCENT);

      char buf[16];
      fmtThousands(st.topVal[i], buf, sizeof(buf));
      g->setTextDatum(middle_right);
      g->setTextColor(C_DIM, C_BG);
      g->drawString(buf, 316, oy + ROW_H / 2);
    }
    if (g == &sprRow) sprRow.pushSprite(0, ROW_Y0 + i * ROW_H);
  }

  if (st.topCount == 0) {
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(C_DIM, C_BG);
    lcd.drawString("Nenhuma tecla registrada ainda", 160, 130);
  }
}

// ---------------------------------------------------------------- tela MAPA DE CALOR
// escala azul -> ciano -> verde -> amarelo -> vermelho
static uint16_t heatColor(uint8_t v) {
  static const uint8_t stops[][4] = {   // posicao, R, G, B
    {0, 20, 30, 90}, {64, 0, 110, 255}, {128, 0, 220, 170}, {192, 255, 220, 0}, {255, 255, 40, 40},
  };
  for (uint8_t i = 1; i < 5; i++) {
    if (v <= stops[i][0]) {
      const uint8_t *a = stops[i - 1], *b = stops[i];
      int t = (v - a[0]) * 256 / (b[0] - a[0]);
      return lcd.color565(a[1] + (b[1] - a[1]) * t / 256,
                          a[2] + (b[2] - a[2]) * t / 256,
                          a[3] + (b[3] - a[3]) * t / 256);
    }
  }
  return lcd.color565(255, 40, 40);
}

static void drawHeatRows() {
  for (uint8_t r = 0; r < LAYOUT_ROW_COUNT; r++) {
    const LayoutRow &row = LAYOUT_ROWS[r];
    LovyanGFX *g = sprRow.getBuffer() ? (LovyanGFX *)&sprRow : (LovyanGFX *)&lcd;
    int oy = (g == &lcd) ? row.y : 0;

    g->fillRect(0, oy, 320, row.h, C_BG);
    for (uint8_t i = 0; i < keyCount; i++) {
      if (keys[i].row != r) continue;
      uint8_t v = heat[heatMode][i];
      uint16_t bg = v ? heatColor(v) : C_PANEL;
      uint16_t fg = !v ? C_DIM : (v > 110 ? (uint16_t)0x0000 : C_TEXT);
      int x = keys[i].x + 1, w = keys[i].w - 2;
      g->fillRoundRect(x, oy, w, row.h, 3, bg);
      if (keys[i].label[0])
        drawKeyLabel(g, keys[i].label, x + w / 2, oy + row.h / 2, fg, bg, false);
    }
    if (g == &sprRow) sprRow.pushSprite(0, row.y);
  }
}

static const uint16_t HEAT_MODE_COLORS[HEAT_MODES] = {C_ACCENT, C_OK, C_ACC2};

static void drawHeatLegend() {
  LovyanGFX *g = sprRow.getBuffer() ? (LovyanGFX *)&sprRow : (LovyanGFX *)&lcd;

  // linha 1: modo atual + escala de cores + dica
  int oy = (g == &lcd) ? 188 : 0;
  g->fillRect(0, oy, 320, 26, C_BG);
  g->fillRoundRect(4, oy + 4, 86, 18, 4, HEAT_MODE_COLORS[heatMode]);
  g->setFont(&fonts::Font2);
  g->setTextDatum(middle_center);
  g->setTextColor(0x0000, HEAT_MODE_COLORS[heatMode]);
  g->drawString(HEAT_MODE_LABELS[heatMode], 47, oy + 13);
  for (int x = 0; x < 110; x++) g->drawFastVLine(100 + x, oy + 8, 10, heatColor(1 + x * 254 / 109));
  g->setFont(&fonts::Font0);
  g->setTextDatum(middle_right);
  g->setTextColor(C_DIM, C_BG);
  g->drawString("segure: trocar", 316, oy + 13);
  if (g == &sprRow) sprRow.pushSprite(0, 188);

  // linha 2: tecla mais usada no modo atual
  oy = (g == &lcd) ? 214 : 0;
  g->fillRect(0, oy, 320, 26, C_BG);
  g->setFont(&fonts::Font2);
  g->setTextDatum(middle_center);
  char buf[80], num[16];
  const char *top = nullptr;
  for (uint8_t i = 0; i < keyCount && !top; i++)
    if (heat[heatMode][i] == 255) top = keys[i].label[0] ? keys[i].label : keys[i].name;
  if (top && strcmp(top, "CCED") == 0) top = "C cedilha";
  else if (top && strcmp(top, "ACUTE") == 0) top = "acento";

  if (heatMode == HEAT_GAME && !heatGame[0]) {
    snprintf(buf, sizeof(buf), "nenhum jogo em foco");
  } else if (heatMax[heatMode]) {
    fmtThousands(heatMax[heatMode], num, sizeof(num));
    if (heatMode == HEAT_GAME)
      snprintf(buf, sizeof(buf), "%.18s: %s (%s)", heatGame, top ? top : "-", num);
    else
      snprintf(buf, sizeof(buf), "%s  |  mais usada: %s (%s)", LAYOUT_NAME, top ? top : "-", num);
  } else {
    snprintf(buf, sizeof(buf), "%s  |  aguardando dados...", LAYOUT_NAME);
  }
  g->setTextColor(C_DIM, C_BG);
  g->drawString(buf, 160, oy + 12);
  if (g == &sprRow) sprRow.pushSprite(0, 214);
}

// ---------------------------------------------------------------- tela POR JOGO
static void fmtFocus(uint32_t s, char *out, size_t n) {
  if (s < 60)        snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum %02lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
  else               snprintf(out, n, "%luh %02lum", (unsigned long)(s / 3600),
                              (unsigned long)((s / 60) % 60));
}

static const int GAME_BOX_W = 154, GAME_ROW_Y0 = 132, GAME_ROW_H = 20;

static void drawGameTitle() {
  lcd.fillRect(0, 24, 320, 32, C_BG);
  lcd.setTextDatum(middle_left);
  if (st.gName[0]) {
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.setTextColor(C_ACCENT, C_BG);
    lcd.setClipRect(6, 24, 308, 32);
    lcd.drawString(st.gName, 6, 41);
    lcd.clearClipRect();
  } else {
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_DIM, C_BG);
    lcd.drawString("Nenhum jogo em foco", 6, 41);
  }
}

static void drawGameDynamic() {
  if (gameTitleDirty) { gameTitleDirty = false; drawGameTitle(); }

  char buf[16];
  fmtThousands(st.gTotal, buf, sizeof(buf));
  drawStatBoxAt(4, 58, GAME_BOX_W, "TECLAS NO JOGO", buf, C_ACCENT);
  fmtThousands(st.gSess, buf, sizeof(buf));
  drawStatBoxAt(162, 58, GAME_BOX_W, "NESTA SESSAO", buf, C_TEXT);
  fmtThousands(st.gPeak, buf, sizeof(buf));
  drawStatBoxAt(4, 94, GAME_BOX_W, "RECORDE (KPM)", buf, C_ACC2);
  fmtFocus(st.gTime, buf, sizeof(buf));
  drawStatBoxAt(162, 94, GAME_BOX_W, "TEMPO EM FOCO", buf, C_OK);

  // top 5 teclas do jogo (o sprite tem 26 px; cada linha cobre a sobra da anterior)
  uint32_t maxV = (st.gTopCount && st.gTopVal[0]) ? st.gTopVal[0] : 1;
  for (uint8_t i = 0; i < GAME_TOP_N; i++) {
    LovyanGFX *g = sprRow.getBuffer() ? (LovyanGFX *)&sprRow : (LovyanGFX *)&lcd;
    int oy = (g == &lcd) ? GAME_ROW_Y0 + i * GAME_ROW_H : 0;
    int h = (g == &lcd) ? GAME_ROW_H : 26;
    g->fillRect(0, oy, 320, h, C_BG);
    if (i < st.gTopCount) {
      g->fillRoundRect(4, oy + 1, 50, GAME_ROW_H - 2, 3, C_PANEL);
      drawKeyLabel(g, labelFor(st.gTopKey[i]), 29, oy + GAME_ROW_H / 2, C_TEXT, C_PANEL, true);
      const int bx = 60, bw = 190;
      int w = (int)((uint64_t)st.gTopVal[i] * bw / maxV);
      if (w < 2) w = 2;
      g->fillRoundRect(bx, oy + 5, bw, GAME_ROW_H - 10, 3, C_PANEL);
      g->fillRoundRect(bx, oy + 5, w,  GAME_ROW_H - 10, 3, i == 0 ? C_ACC2 : C_ACCENT);
      fmtThousands(st.gTopVal[i], buf, sizeof(buf));
      g->setFont(&fonts::Font2);
      g->setTextDatum(middle_right);
      g->setTextColor(C_DIM, C_BG);
      g->drawString(buf, 316, oy + GAME_ROW_H / 2);
    } else if (i == 0) {
      g->setFont(&fonts::Font2);
      g->setTextDatum(middle_center);
      g->setTextColor(C_DIM, C_BG);
      g->drawString("Nenhuma tecla neste jogo ainda", 160, oy + GAME_ROW_H / 2);
    }
    if (g == &sprRow) sprRow.pushSprite(0, GAME_ROW_Y0 + i * GAME_ROW_H);
  }
}

// ---------------------------------------------------------------- navegacao
static void redrawAll() {
  drawHeader();
  if (!linkUp && !everConnected) { drawWaiting(); return; }
  lcd.fillRect(0, 24, 320, 216, C_BG);
  if (screen == SCR_LIVE)      { drawLiveDynamic(); drawGraph(); }
  else if (screen == SCR_GAME) { gameTitleDirty = true; drawGameDynamic(); }
  else if (screen == SCR_TOP)  { drawTopRows(); }
  else                         { drawHeatRows(); drawHeatLegend(); }
}

static void nextScreen() {
  screen = (Screen)((screen + 1) % SCR_COUNT);
  topDirty = heatDirty = false;  // a tela inteira e redesenhada abaixo
  redrawAll();
}

enum InputEvent : uint8_t { IN_NONE, IN_TAP, IN_LONG };

// Toque/clique curto = IN_TAP; toque longo (1 s) ou BOOT segurado por 1-3 s = IN_LONG;
// BOOT segurado por mais de 3 s = abre o assistente do display.
static InputEvent pollInput() {
  static bool     wasPressed = false, touchActive = false, longFired = false;
  static uint32_t bootDownAt = 0, touchStart = 0, touchSeen = 0, lastTap = 0;

  int32_t tx, ty;
  bool touched = lcd.getTouch(&tx, &ty) > 0;
  bool pressed = digitalRead(PIN_BOOT) == LOW;
  uint32_t now = millis();
  InputEvent ev = IN_NONE;

  // botao BOOT (avaliado ao soltar)
  if (pressed && !wasPressed) bootDownAt = now;
  if (pressed && now - bootDownAt > 3000) {
    prefs.putBool("dok", false);
    delay(50);
    ESP.restart();
  }
  if (!pressed && wasPressed) ev = (now - bootDownAt < LONG_PRESS_MS) ? IN_TAP : IN_LONG;
  wasPressed = pressed;

  // touch: o XPT2046 "pisca" durante o toque, entao so considera solto apos TOUCH_UP_MS
  if (touched) {
    touchSeen = now;
    if (!touchActive) {
      touchActive = true;
      longFired = false;
      touchStart = now;
    } else if (!longFired && now - touchStart >= LONG_PRESS_MS) {
      longFired = true;
      ev = IN_LONG;  // dispara ainda segurando, para dar retorno imediato
    }
  } else if (touchActive && now - touchSeen > TOUCH_UP_MS) {
    touchActive = false;
    if (!longFired) ev = IN_TAP;
  }

  if (ev == IN_TAP) {
    if (now - lastTap < 250) ev = IN_NONE;
    else lastTap = now;
  }
  return ev;
}

// ---------------------------------------------------------------- assistente do display
static const uint8_t ROTATIONS[] = {1, 5, 3, 7};  // normal, espelhada, 180 graus, 180 espelhada

static void drawSetupScreen(uint8_t prof, uint8_t rotIdx) {
  lcd.fillScreen(C_BG);
  lcd.drawRect(0, 0, 320, 240, C_TEXT);       // a moldura deve encostar nas 4 bordas
  lcd.drawRect(2, 2, 316, 236, C_DIM);

  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextColor(C_ACCENT, C_BG);
  lcd.drawString("CONFIGURAR DISPLAY", 12, 10);

  char buf[64];
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(C_TEXT, C_BG);
  snprintf(buf, sizeof(buf), "Perfil %u/%u: %s", prof + 1, DISPLAY_PROFILE_COUNT,
           DISPLAY_PROFILES[prof].name);
  lcd.drawString(buf, 12, 34);
  snprintf(buf, sizeof(buf), "Orientacao %u/4", rotIdx + 1);
  lcd.drawString(buf, 12, 52);
  lcd.drawString("Este texto le da esquerda p/ direita -->", 12, 70);

  // barras de cor rotuladas: confira se cada cor bate com o nome
  struct { uint16_t c; const char *n; uint16_t fg; } bars[] = {
    {0xF800, "VERMELHO", 0xFFFF}, {0x07E0, "VERDE", 0x0000},
    {0x001F, "AZUL", 0xFFFF},     {0xFFFF, "BRANCO", 0x0000},
  };
  for (int i = 0; i < 4; i++) {
    int x = 12 + i * 75;
    lcd.fillRect(x, 94, 70, 34, bars[i].c);
    lcd.setFont(&fonts::Font0);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(bars[i].fg, bars[i].c);
    lcd.drawString(bars[i].n, x + 35, 111);
  }
  lcd.fillRect(12, 132, 296, 8, 0x0000);  // preto deve ser preto (nao branco)

  lcd.setFont(&fonts::Font2);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(C_DIM, C_BG);
  lcd.drawString("Aperte BOOT ....... proximo perfil", 12, 150);
  lcd.drawString("Toque na tela ..... girar / espelhar", 12, 170);
  lcd.setTextColor(C_OK, C_BG);
  lcd.drawString("Segure BOOT 2 s ... salvar e usar", 12, 190);
  lcd.setTextColor(C_DIM, C_BG);
  lcd.drawString("Tudo certo = moldura nas 4 bordas,", 12, 210);
}

// Bloqueia ate o usuario salvar. Trocar de perfil reinicia a placa.
static void runDisplaySetup(uint8_t prof, uint8_t rot) {
  setLed(true, false, true);  // roxo = modo de configuracao
  uint8_t ri = 0;
  for (uint8_t i = 0; i < 4; i++) if (ROTATIONS[i] == rot) ri = i;

  Serial.println(F("[display] Assistente de configuracao:"));
  Serial.println(F("  BOOT = proximo perfil | toque = girar/espelhar | segure BOOT 2 s = salvar"));
  Serial.printf("[display] perfil %u: %s, rotacao %u\n", prof + 1, DISPLAY_PROFILES[prof].name, rot);

  lcd.setRotation(ROTATIONS[ri]);
  drawSetupScreen(prof, ri);

  bool wasTouched = false;
  uint32_t lastTouch = 0;
  while (true) {
    if (digitalRead(PIN_BOOT) == LOW) {
      uint32_t t0 = millis();
      bool saved = false;
      while (digitalRead(PIN_BOOT) == LOW) {
        if (!saved && millis() - t0 > 2000) {
          saved = true;
          prefs.putUChar("drot", ROTATIONS[ri]);
          prefs.putBool("dok", true);
          lcd.fillRect(60, 95, 200, 50, C_OK);
          lcd.setFont(&fonts::FreeSansBold12pt7b);
          lcd.setTextDatum(middle_center);
          lcd.setTextColor(0x0000, C_OK);
          lcd.drawString("SALVO!", 160, 120);
          Serial.println(F("[display] configuracao salva"));
        }
        delay(10);
      }
      if (saved) { lcd.fillScreen(C_BG); return; }
      if (millis() - t0 > 30) {  // toque curto: proximo perfil
        prefs.putUChar("dprof", (prof + 1) % DISPLAY_PROFILE_COUNT);
        prefs.putUChar("drot", ROTATIONS[ri]);
        delay(50);
        ESP.restart();
      }
    }

    int32_t tx, ty;
    bool touched = lcd.getTouch(&tx, &ty) > 0;
    if (touched && !wasTouched && millis() - lastTouch > 400) {
      lastTouch = millis();
      ri = (ri + 1) % 4;
      lcd.setRotation(ROTATIONS[ri]);
      drawSetupScreen(prof, ri);
      Serial.printf("[display] rotacao %u\n", ROTATIONS[ri]);
    }
    wasTouched = touched;
    delay(20);
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(SERIAL_BAUD);

  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  setLed(false, false, true);  // azul = aguardando

  // perfil do display salvo na flash (ver lgfx_cyd.h)
  prefs.begin("keystats", false);
  uint8_t prof = prefs.getUChar("dprof", 0);
  if (prof >= DISPLAY_PROFILE_COUNT) prof = 0;
  uint8_t rot = prefs.getUChar("drot", 1);
  bool displayOk = prefs.getBool("dok", false);

  lcd.configure(prof);
  lcd.init();
  lcd.setRotation(rot);        // paisagem, 320x240
  lcd.setBrightness(200);
  lcd.fillScreen(C_BG);

  if (!displayOk) runDisplaySetup(prof, rot);
  heatMode = prefs.getUChar("hmode", HEAT_TOTAL) % HEAT_MODES;
  setLed(false, false, true);

  // sprites evitam o "pisca" ao redesenhar; se faltar RAM, desenha direto na tela
  sprKpm.setColorDepth(16);
  sprKpm.createSprite(KPM_W, KPM_H);
  sprGraph.setColorDepth(16);
  sprGraph.createSprite(GR_W, GR_H);
  sprRow.setColorDepth(16);
  sprRow.createSprite(320, ROW_H);
  sprHeader.setColorDepth(16);
  sprHeader.createSprite(320, 24);

  memset(hist, 0, sizeof(hist));
  memset(heat, 0, sizeof(heat));
  memset(heatMax, 0, sizeof(heatMax));
  buildLayout();
  redrawAll();
}

void loop() {
  pollSerial();
  uint32_t now = millis();

  // estado da conexao
  bool up = everConnected && (now - lastMsgMs < LINK_TIMEOUT_MS);
  if (up != linkUp) {
    linkUp = up;
    if (up) setLed(false, true, false);   // verde = recebendo dados
    else    setLed(true, false, false);   // vermelho = perdeu o PC
    if (!up) st.kpm = 0;
    redrawAll();
  }

  InputEvent ev = pollInput();
  if (ev == IN_TAP) {
    nextScreen();
  } else if (ev == IN_LONG && screen == SCR_HEAT && everConnected) {
    heatMode = (heatMode + 1) % HEAT_MODES;
    prefs.putUChar("hmode", heatMode);
    heatDirty = false;
    drawHeatRows();
    drawHeatLegend();
  }

  // historico: uma amostra por segundo
  static uint32_t lastSample = 0;
  if (now - lastSample >= 1000) {
    lastSample = now;
    hist[histHead] = linkUp ? (uint16_t)min<uint32_t>(st.kpm, 65535) : 0;
    histHead = (histHead + 1) % HIST_N;
    if (everConnected && screen == SCR_LIVE) drawGraph();
  }

  // redesenho limitado a ~10 FPS
  static uint32_t lastDraw = 0;
  if (dirty && now - lastDraw >= 100) {
    lastDraw = now;
    dirty = false;
    drawHeader();
    if (screen == SCR_LIVE) {
      drawLiveDynamic();
    } else if (screen == SCR_GAME) {
      drawGameDynamic();
    } else if (screen == SCR_TOP && topDirty) {
      topDirty = false;
      drawTopRows();
    } else if (screen == SCR_HEAT && heatDirty) {
      heatDirty = false;
      drawHeatRows();
      drawHeatLegend();
    }
  }
}
