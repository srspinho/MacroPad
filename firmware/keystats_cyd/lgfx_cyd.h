// Configuracao do LovyanGFX para o "Cheap Yellow Display" (ESP32-2432S028R).
//
// Existem varias revisoes da placa com controladores de display diferentes
// (ILI9341, "ILI9341 tipo 2" e ST7789) e com cores invertidas ou nao. Em vez de
// fixar um no codigo, o firmware escolhe um PERFIL em tempo de execucao e o
// guarda na memoria flash (NVS). O assistente de configuracao (keystats_cyd.ino)
// deixa voce testar os perfis direto na placa, sem regravar o firmware.

#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

enum CydPanelType : uint8_t { CYD_ILI9341 = 0, CYD_ILI9341_2, CYD_ST7789 };

struct DisplayProfile {
  CydPanelType panel;
  bool         invert;  // inversao de cores
  bool         bgr;     // ordem RGB/BGR
  const char  *name;
};

// Ordem = do mais comum para o menos comum
static const DisplayProfile DISPLAY_PROFILES[] = {
  {CYD_ILI9341,   false, false, "ILI9341"},                   // CYD original (micro-USB)
  {CYD_ILI9341_2, false, false, "ILI9341 tipo 2"},            // revisoes com USB-C
  {CYD_ILI9341_2, true,  false, "ILI9341 tipo 2 invertido"},
  {CYD_ST7789,    false, false, "ST7789"},                    // "CYD2USB"
  {CYD_ST7789,    true,  false, "ST7789 invertido"},
  {CYD_ILI9341,   true,  false, "ILI9341 invertido"},
  {CYD_ST7789,    false, true,  "ST7789 BGR"},
  {CYD_ILI9341,   false, true,  "ILI9341 BGR"},
};
static const uint8_t DISPLAY_PROFILE_COUNT = sizeof(DISPLAY_PROFILES) / sizeof(DISPLAY_PROFILES[0]);

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341   _ili9341;
  lgfx::Panel_ILI9341_2 _ili9341_2;
  lgfx::Panel_ST7789    _st7789;
  lgfx::Bus_SPI         _bus;
  lgfx::Light_PWM       _light;
  lgfx::Touch_XPT2046   _touch;

 public:
  // Chame antes de init(). profile = indice em DISPLAY_PROFILES.
  void configure(uint8_t profile) {
    if (profile >= DISPLAY_PROFILE_COUNT) profile = 0;
    const DisplayProfile &p = DISPLAY_PROFILES[profile];

    lgfx::Panel_Device *panel =
        p.panel == CYD_ST7789    ? (lgfx::Panel_Device *)&_st7789
      : p.panel == CYD_ILI9341_2 ? (lgfx::Panel_Device *)&_ili9341_2
                                 : (lgfx::Panel_Device *)&_ili9341;

    {  // barramento SPI do display (HSPI)
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 14;
      cfg.pin_mosi    = 13;
      cfg.pin_miso    = 12;
      cfg.pin_dc      = 2;
      _bus.config(cfg);
      panel->setBus(&_bus);
    }
    {  // painel
      auto cfg = panel->config();
      cfg.pin_cs          = 15;
      cfg.pin_rst         = -1;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 240;
      cfg.panel_height    = 320;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;
      cfg.readable        = true;
      cfg.invert          = p.invert;
      cfg.rgb_order       = p.bgr;
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      panel->config(cfg);
    }
    {  // backlight
      auto cfg = _light.config();
      cfg.pin_bl      = 21;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      panel->setLight(&_light);
    }
    {  // touch resistivo XPT2046 (em barramento SPI proprio, VSPI)
      auto cfg = _touch.config();
      cfg.x_min           = 300;
      cfg.x_max           = 3900;
      cfg.y_min           = 200;
      cfg.y_max           = 3700;
      cfg.pin_int         = 36;
      cfg.bus_shared      = false;
      cfg.offset_rotation = 0;
      cfg.spi_host        = SPI3_HOST;
      cfg.freq            = 1000000;
      cfg.pin_sclk        = 25;
      cfg.pin_mosi        = 32;
      cfg.pin_miso        = 39;
      cfg.pin_cs          = 33;
      _touch.config(cfg);
      panel->setTouch(&_touch);
    }
    setPanel(panel);
  }
};
