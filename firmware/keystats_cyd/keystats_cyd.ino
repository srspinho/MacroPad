/*
  KeyStats Panel - firmware MVP para o "Cheap Yellow Display" (ESP32-2432S028R)

  Recebe estatisticas de teclado do agente no PC (keystats_agent.py) via USB serial,
  uma linha JSON por mensagem, e mostra em duas telas:
    1) AO VIVO   - KPM atual, pico, teclas da sessao, total e grafico dos ultimos 60 s
    2) TOP TECLAS - as 8 teclas mais usadas
    3) MAPA DE CALOR - o teclado colorido pela frequencia de uso (ABNT2 ou ANSI)

  Toque na tela (ou aperte o botao BOOT) para trocar de tela.

  Bibliotecas (Gerenciador de Bibliotecas da IDE):
    - LovyanGFX   (testado com 1.2.0)
    - ArduinoJson (testado com 7.2.1)
  Placa: "ESP32 Dev Module" (core esp32 da Espressif 3.x)

  Protocolo (115200 baud, uma linha JSON terminada em '\n'):
    {"kpm":312,"peak":540,"sess":1834,"total":184233,"up":3605,
     "app":"cs2.exe","top":[["W",5123],["A",3301],["SPACE",2980]]}
  e, a cada ~2 s, uma linha com o mapa de calor (intensidade 1..255 por tecla):
    {"heat":{"W":255,"A":204,"SPACE":180},"hmax":5123}
*/

#include "lgfx_cyd.h"
#include "keyboard_layout.h"
#include <ArduinoJson.h>

// ---------------------------------------------------------------- configuracao
static const uint32_t SERIAL_BAUD     = 115200;
static const uint32_t LINK_TIMEOUT_MS = 3000;   // sem dados por esse tempo = desconectado
static const uint8_t  TOP_N           = 8;
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
};

static LGFX        lcd;
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

enum Screen : uint8_t { SCR_LIVE = 0, SCR_TOP, SCR_HEAT, SCR_COUNT };
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
static uint8_t  heat[MAX_KEYS];
static uint32_t heatMax   = 0;
static bool     heatDirty = true;
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
    memset(heat, 0, sizeof(heat));
    for (JsonPair kv : heatObj) {
      const char *k = kv.key().c_str();
      uint8_t v = kv.value() | 0;
      for (uint8_t i = 0; i < keyCount; i++)   // sem break: ENTER ISO ocupa 2 retangulos
        if (strcmp(keys[i].name, k) == 0) heat[i] = v;
    }
    heatMax = doc["hmax"] | 0;
    heatDirty = true;
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

static void drawStatBox(int y, const char *label, uint32_t value, uint16_t color) {
  char buf[16];
  fmtThousands(value, buf, sizeof(buf));
  lcd.fillRoundRect(BOX_X, y, BOX_W, BOX_H, 4, C_PANEL);
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(C_DIM, C_PANEL);
  lcd.drawString(label, BOX_X + 6, y + 3);
  lcd.setFont(&fonts::Font2);
  lcd.setTextDatum(bottom_right);
  lcd.setTextColor(color, C_PANEL);
  lcd.drawString(buf, BOX_X + BOX_W - 6, y + BOX_H - 2);
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
      uint8_t v = heat[i];
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

static void drawHeatLegend() {
  LovyanGFX *g = sprRow.getBuffer() ? (LovyanGFX *)&sprRow : (LovyanGFX *)&lcd;
  // linha 1: barra de cores
  int oy = (g == &lcd) ? 188 : 0;
  g->fillRect(0, oy, 320, 26, C_BG);
  for (int x = 0; x < 200; x++) g->drawFastVLine(60 + x, oy + 8, 10, heatColor(1 + x * 254 / 199));
  g->setFont(&fonts::Font0);
  g->setTextColor(C_DIM, C_BG);
  g->setTextDatum(middle_right);
  g->drawString("pouco", 55, oy + 13);
  g->setTextDatum(middle_left);
  g->drawString("muito", 265, oy + 13);
  if (g == &sprRow) sprRow.pushSprite(0, 188);

  // linha 2: legenda
  oy = (g == &lcd) ? 214 : 0;
  g->fillRect(0, oy, 320, 26, C_BG);
  g->setFont(&fonts::Font2);
  g->setTextDatum(middle_center);
  char buf[64], num[16];
  if (heatMax && st.topCount) {
    fmtThousands(heatMax, num, sizeof(num));
    const char *lbl = labelFor(st.topKey[0]);
    if (strcmp(lbl, "CCED") == 0) lbl = "C cedilha";
    else if (strcmp(lbl, "ACUTE") == 0) lbl = "acento";
    snprintf(buf, sizeof(buf), "%s  |  mais usada: %s (%s)", LAYOUT_NAME, lbl, num);
  } else {
    snprintf(buf, sizeof(buf), "%s  |  aguardando dados...", LAYOUT_NAME);
  }
  g->setTextColor(C_DIM, C_BG);
  g->drawString(buf, 160, oy + 12);
  if (g == &sprRow) sprRow.pushSprite(0, 214);
}

// ---------------------------------------------------------------- navegacao
static void redrawAll() {
  drawHeader();
  if (!linkUp && !everConnected) { drawWaiting(); return; }
  lcd.fillRect(0, 24, 320, 216, C_BG);
  if (screen == SCR_LIVE)     { drawLiveDynamic(); drawGraph(); }
  else if (screen == SCR_TOP) { drawTopRows(); }
  else                        { drawHeatRows(); drawHeatLegend(); }
}

static void nextScreen() {
  screen = (Screen)((screen + 1) % SCR_COUNT);
  topDirty = heatDirty = false;  // a tela inteira e redesenhada abaixo
  redrawAll();
}

static bool pollInput() {
  static bool wasTouched = false, wasPressed = false;
  static uint32_t lastChange = 0;
  bool fired = false;

  int32_t tx, ty;
  bool touched = lcd.getTouch(&tx, &ty) > 0;
  bool pressed = digitalRead(PIN_BOOT) == LOW;
  uint32_t now = millis();

  if (now - lastChange > 250) {
    if ((touched && !wasTouched) || (pressed && !wasPressed)) {
      fired = true;
      lastChange = now;
    }
  }
  wasTouched = touched;
  wasPressed = pressed;
  return fired;
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

  lcd.init();
  lcd.setRotation(1);          // paisagem, 320x240
  lcd.setBrightness(200);
  lcd.fillScreen(C_BG);

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

  if (pollInput()) nextScreen();

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
