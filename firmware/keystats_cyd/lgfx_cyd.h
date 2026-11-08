// Configuracao do LovyanGFX para o "Cheap Yellow Display" (ESP32-2432S028R).
//
// Ha variantes da placa no mercado:
//  - Versao original (1 porta micro-USB): controlador ILI9341  -> padrao
//  - Versao "2 USB" (micro-USB + USB-C):  controlador ST7789   -> descomente CYD_PANEL_ST7789
// Se as cores aparecerem invertidas (fundo branco, cores "negativas"),
// alterne CYD_INVERT_COLORS.

#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// #define CYD_PANEL_ST7789
// #define CYD_INVERT_COLORS

class LGFX : public lgfx::LGFX_Device {
#ifdef CYD_PANEL_ST7789
  lgfx::Panel_ST7789 _panel;
#else
  lgfx::Panel_ILI9341 _panel;
#endif
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;

 public:
  LGFX() {
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
      _panel.setBus(&_bus);
    }
    {  // painel
      auto cfg = _panel.config();
      cfg.pin_cs          = 15;
      cfg.pin_rst         = -1;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 240;
      cfg.panel_height    = 320;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;
      cfg.readable        = true;
#ifdef CYD_INVERT_COLORS
      cfg.invert          = true;
#else
      cfg.invert          = false;
#endif
      cfg.rgb_order       = false;
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      _panel.config(cfg);
    }
    {  // backlight
      auto cfg = _light.config();
      cfg.pin_bl      = 21;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
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
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
