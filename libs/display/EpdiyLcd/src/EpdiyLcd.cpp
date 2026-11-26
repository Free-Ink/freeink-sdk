/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

// Board power hooks, 1 bpp/overlay-mask conversion and refresh transactions.
// Vendored epdiy retains its LGPL licence; local resource/baseline fixes are
// exercised by test/host/test_transactions.py.

#include <EpdiyLcd.h>
#include <esp_heap_caps.h>
#if FREEINK_READPICO_DIAGNOSTICS
#include <esp_timer.h>
// rom console rather than ESP_LOGI: on this build ESP_LOG does not reach the serial
// port. The header is ESP-IDF-only, and the host transaction test builds this file
// twice -- once with diagnostics on -- so it cannot be included unconditionally.
// Declare the entry point instead and let the test supply the definition.
#if defined(ESP_PLATFORM)
#include <esp_rom_sys.h>
#else
extern "C" int esp_rom_printf(const char* format, ...);
#endif
#endif
#include <esp_log.h>

#include <cstring>

extern "C" {
// 未带 extern "C" 守卫的 epdiy 头（见本库 README 的清单）在这里统一包住；
// 已带守卫的再包一层是合法的。
// / epdiy headers without extern "C" guards are wrapped here; re-wrapping the
// guarded ones is harmless.
#include "e0470/include/e0470_epaper_waveform.h"
#include "epdiy/include/epd_lcd.h"
#include "epdiy/include/epd_waveform.h"
#include "epdiy/src/epd_board.h"
#include "epdiy/src/epd_display.h"
#include "epdiy/src/epd_highlevel.h"
#include "epdiy/src/epdiy.h"
}

namespace freeink {
namespace {

const EpdiyLcdConfig* g_cfg = nullptr;
EpdiyHighlevelState g_hl = {};
uint8_t* g_fb4 = nullptr;
// 最近一次推上去的黑白页（1bpp）。AA 的灰度提交需要它当底图，因为那时调用方的缓冲
// 装的是选择平面而不是页面。/ The last B/W page pushed (1 bpp). The AA gray commit
// needs it as a base because the caller's buffer then holds a selector plane.
uint8_t* g_base = nullptr;
bool g_started = false;
bool g_nativeGrayActive = false;
bool g_initialized = false;
bool g_powerReady = false;
bool g_baselineKnown = false;
// 调用方的 1bpp 位约定，由 epdiyLcdBegin 给定一次。
// / The caller's 1 bpp bit convention, fixed once by epdiyLcdBegin.
bool g_blackIsOne = false;

// 1bpp 行 → 4bpp 行的展开表：一个输入字节出 4 个输出字节。
// epdiy 是每字节两个像素，偶数列在低半字节、奇数列在高半字节，15 = 白。
// / Expansion table: one 1 bpp input byte becomes four 4 bpp bytes. epdiy packs
// two pixels per byte, even column in the low nibble, odd in the high, 15 = white.
uint32_t g_expand[2][256];

void buildExpandTable() {
  // index 0: 置位 = 黑 (level 0)，清零 = 白 (level 15)
  // index 1: 置位 = 白 (level 15)，清零 = 黑 (level 0)
  for (int oneIsBlack = 0; oneIsBlack < 2; ++oneIsBlack) {
    for (int b = 0; b < 256; ++b) {
      uint8_t out[4];
      for (int pair = 0; pair < 4; ++pair) {
        uint8_t lvl[2];
        for (int k = 0; k < 2; ++k) {
          const int bit = (b >> (7 - (pair * 2 + k))) & 1;
          const int black = oneIsBlack ? bit : (bit ^ 1);
          lvl[k] = static_cast<uint8_t>(black ? 0 : 15);
        }
        out[pair] = static_cast<uint8_t>(lvl[0] | (lvl[1] << 4));
      }
      g_expand[oneIsBlack][b] = static_cast<uint32_t>(out[0]) | (static_cast<uint32_t>(out[1]) << 8) |
                                (static_cast<uint32_t>(out[2]) << 16) | (static_cast<uint32_t>(out[3]) << 24);
    }
  }
}

float panelTemperature() {
  if (g_cfg != nullptr && g_cfg->power.getTemperature != nullptr) {
    const float t = g_cfg->power.getTemperature();
    if (t >= 0.0f && t <= 50.0f) return t;
  }
  // 波形只有一个 0–50 °C 档，所以选表结果与具体值无关。
  // / The table has a single 0–50 °C range, so the exact value is irrelevant.
  return 20.0f;
}

// --- EpdBoardDefinition -----------------------------------------------------

bool boardInit(uint32_t epdRowWidth) {
  (void)epdRowWidth;  // 行宽由 lcd_bus_config_t + 面板宽度决定 / from the bus config
  if (g_cfg == nullptr) return false;

  // 引脚先归到安全电平（厂商 board_init 的顺序：pinMode 全部先做，再让 LCD 外设接管）。
  // / Park the pins first, exactly like the reference board_init(), then hand them
  // to the LCD peripheral.
  if (g_cfg->power.prepare != nullptr && !g_cfg->power.prepare()) return false;

  LcdEpdConfig_t c = {};
  c.pixel_clock = static_cast<size_t>(g_cfg->pclkMhz) * 1000u * 1000u;
  c.line.le_high_time = g_cfg->line.leHighTime;
  c.line.line_front_porch = g_cfg->line.lineFrontPorch;
  c.line.line_end = g_cfg->line.lineEnd;
  c.line.ckv_high_time = g_cfg->line.ckvHighTime01us;
  c.bus_width = g_cfg->busWidth;
  for (int i = 0; i < 16; ++i) c.bus.data[i] = static_cast<gpio_num_t>(g_cfg->dataPins[i]);
  c.bus.clock = static_cast<gpio_num_t>(g_cfg->pinClock);
  c.bus.ckv = static_cast<gpio_num_t>(g_cfg->pinCkv);
  c.bus.start_pulse = static_cast<gpio_num_t>(g_cfg->pinStartPulse);
  c.bus.leh = static_cast<gpio_num_t>(g_cfg->pinLeh);
  c.bus.stv = static_cast<gpio_num_t>(g_cfg->pinStv);

  if (epd_lcd_init(&c, static_cast<int>(epd_width()), static_cast<int>(epd_height())) != ESP_OK) return false;
  epd_lcd_set_prefill_lines(g_cfg->prefillLines);
  return true;
}

void boardDeinit() { epd_lcd_deinit(); }

// XOE 与 MODE 在 FCA9555 上，由 powerOn/powerOff 按顺序持有；本板没有 epdiy 假设的
// 那个通用控制寄存器，所以这里是空实现。
// / XOE and MODE live on the FCA9555 and are sequenced by powerOn/powerOff. This
// board has no equivalent of epdiy's generic control register, so this is a no-op.
void boardSetCtrl(epd_ctrl_state_t* state, const epd_ctrl_state_t* const mask) {
  (void)state;
  (void)mask;
}

void boardPowerOn(epd_ctrl_state_t* state) {
  (void)state;
  g_powerReady = g_cfg != nullptr && g_cfg->power.powerOn != nullptr && g_cfg->power.powerOn();
}

void boardPowerOff(epd_ctrl_state_t* state) {
  (void)state;
  if (g_cfg != nullptr && g_cfg->power.powerOff != nullptr) g_cfg->power.powerOff();
  g_powerReady = false;
}

void boardMeasureVcom(epd_ctrl_state_t* state) { (void)state; }

// 硬规则：面板 VCOM 是出厂写进 PMU 的，与这块玻璃配对，写错会永久损坏面板。
// epdiy 的 v6/v7 板才会实现这个回调；本板必须保持空实现。
// / HARD RULE: the panel VCOM is factory-written in the PMU and paired with this
// glass; a wrong value damages it permanently. Only epdiy's v6/v7 boards implement
// this callback — on this board it must stay a no-op.
void boardSetVcom(int vcomMv) { (void)vcomMv; }

float boardGetTemperature() { return panelTemperature(); }

const EpdBoardDefinition kBoard = {
    /* init            */ boardInit,
    /* deinit          */ boardDeinit,
    /* set_ctrl        */ boardSetCtrl,
    /* poweron         */ boardPowerOn,
    /* measure_vcom    */ boardMeasureVcom,
    /* poweroff        */ boardPowerOff,
    /* set_vcom        */ boardSetVcom,
    /* get_temperature */ boardGetTemperature,
    /* gpio_set_direction */ nullptr,
    /* gpio_read          */ nullptr,
    /* gpio_write         */ nullptr,
};

}  // namespace

bool epdiyLcdBegin(const EpdiyLcdConfig& cfg, uint16_t width, uint16_t height, bool blackIsOne) {
  if (g_started) return g_fb4 != nullptr;
  g_cfg = &cfg;
  g_blackIsOne = blackIsOne;

  // 波形必须在 epd_hl_init 之前建好：E0470_WAVEFORM 的相位数据由它填。
  // / The waveform must be built before epd_hl_init(): it fills E0470_WAVEFORM.
  e0470_waveform_init();

  epd_set_board(&kBoard);
  if (!epd_init(&kBoard, &E0470_DISPLAY, EPD_OPTIONS_DEFAULT)) {
    ESP_LOGE("EpdiyLcd", "LCD renderer initialization failed");
    return false;
  }
  g_initialized = true;

  // 几何校验必须在 epd_init 之后：epd_width()/epd_height() 读的是 epd_init 里
  // `display = disp` 设进去的那张表，在此之前 esp_get_display() 还是 NULL。
  // epdiy 用它自己的 epd_width()/epd_height()（E0470_DISPLAY = 1216x684）扫描，
  // 与 BoardProfile 的几何必须一致，否则扫描与帧缓冲会错位。
  // / The geometry check MUST come after epd_init: epd_width()/epd_height() read the
  // display table that epd_init assigns, and before it epd_get_display() is NULL.
  // epdiy scans using its own epd_width()/epd_height() (E0470_DISPLAY is 1216x684);
  // it must agree with the BoardProfile geometry or scan and framebuffer disagree.
  if (epd_width() != width || epd_height() != height) {
    epdiyLcdEnd();
    return false;
  }

  g_hl = epd_hl_init(&E0470_WAVEFORM);
  g_fb4 = epd_hl_get_framebuffer(&g_hl);
  if (g_fb4 == nullptr) {
    epdiyLcdEnd();
    return false;
  }

  // 1bpp 底图，宽/8 字节每行。/ 1 bpp base image, width/8 bytes per row.
  const size_t baseBytes = static_cast<size_t>(width) / 8 * static_cast<size_t>(height);
  g_base = static_cast<uint8_t*>(heap_caps_malloc(baseBytes, MALLOC_CAP_SPIRAM));
  if (g_base == nullptr) {
    ESP_LOGE("EpdiyLcd", "Base allocation failed (%u bytes)", static_cast<unsigned>(baseBytes));
    epdiyLcdEnd();
    return false;
  }
  memset(g_base, 0xFF, baseBytes);  // 起始为白纸 / starts as white paper

  buildExpandTable();

  // The panel retains its image at boot. Establish a baseline on the first draw.
  g_baselineKnown = false;

  const size_t bufBytes = static_cast<size_t>(width) / 2 * static_cast<size_t>(height);
  // 白场按调用方的位约定展开，极性翻转时不会写错。
  // / The white field is expanded with the caller's bit convention, so it stays
  // correct if the polarity is ever flipped.
  const uint32_t whiteWord = g_expand[blackIsOne ? 0 : 1][0xFF];
  uint32_t* words = reinterpret_cast<uint32_t*>(g_fb4);
  for (size_t i = 0; i < bufBytes / 4; ++i) words[i] = whiteWord;
  memset(g_base, 0xFF, baseBytes);  // facade 约定：置位 = 白 / facade: a set bit is white

  g_started = true;
  return true;
}

void epdiyLcdEnd() {
  if (!g_initialized) return;
  epd_deinit();
  epd_hl_deinit(&g_hl);
  heap_caps_free(g_base);
  g_base = nullptr;
  g_fb4 = nullptr;
  g_started = false;
  g_nativeGrayActive = false;
  g_initialized = false;
  g_baselineKnown = false;
}

namespace {

enum EpdDrawMode drawModeFor(EpdiyLcdRefresh mode) {
  switch (mode) {
    case EpdiyLcdRefresh::Full:
      return static_cast<enum EpdDrawMode>(MODE_GC16 | PREVIOUSLY_WHITE);
    case EpdiyLcdRefresh::Half:
    case EpdiyLcdRefresh::TextTurn:
      return static_cast<enum EpdDrawMode>(MODE_GL16 | PREVIOUSLY_WHITE);
    case EpdiyLcdRefresh::Fast:
    default:
      return static_cast<enum EpdDrawMode>(MODE_DU | PREVIOUSLY_WHITE);
  }
}

// 文字转页换用对角线全保持的那张表：未变化的像素完全不驱动，所以不会把黑像素先擦白
// 再推回黑——那一下擦白就是翻页看到的闪。其余档位沿用 epd_hl_init 挂上的默认表。
// / The text turn swaps in the table whose to == from diagonal is entirely held, so a
// pixel that did not change is not driven at all. Driving it would erase it white first
// and push it back to black, and that erase is the flash a turn shows. Every other
// profile keeps the table epd_hl_init attached.
const EpdWaveform* waveformFor(EpdiyLcdRefresh mode) {
  return mode == EpdiyLcdRefresh::TextTurn ? &E0470_TEXTTURN_WAVEFORM : &E0470_WAVEFORM;
}

// 1bpp 页 -> epdiy 4bpp。展开表把调用方的位约定翻成 epdiy 的灰度级；本玻璃实测
// level 0 呈白、15 呈黑，所以 blackIsOne 的语义见 EpdiyLcd.h 的说明。
// / 1 bpp page -> epdiy 4 bpp. The table translates the caller's bit convention into
// epdiy gray levels. On this glass level 0 renders white and 15 renders black; see
// EpdiyLcd.h for what blackIsOne means.
void fillFrom1bpp(const uint8_t* fb) {
  const uint16_t w = static_cast<uint16_t>(epd_width());
  const uint16_t h = static_cast<uint16_t>(epd_height());
  const size_t srcStride = w / 8;
  const size_t dstStride = w / 2;
  const uint32_t* table = g_expand[g_blackIsOne ? 0 : 1];
  for (uint16_t y = 0; y < h; ++y) {
    const uint8_t* src = fb + static_cast<size_t>(y) * srcStride;
    uint32_t* dst = reinterpret_cast<uint32_t*>(g_fb4 + static_cast<size_t>(y) * dstStride);
    for (size_t i = 0; i < srcStride; ++i) dst[i] = table[src[i]];
  }
}

#if FREEINK_READPICO_DIAGNOSTICS
struct FrameTimingStats {
  uint32_t frames = 0, convertUs = 0, maxConvertUs = 0;
};
FrameTimingStats g_frameTiming;
void recordConversion(int64_t startedUs) {
  g_frameTiming.convertUs = esp_timer_get_time() - startedUs;
  if (g_frameTiming.convertUs > g_frameTiming.maxConvertUs) g_frameTiming.maxConvertUs = g_frameTiming.convertUs;
}
#endif

bool pushFrame(EpdiyLcdRefresh mode, bool turnOff) {
  epd_poweron();
  const auto setScanActive = g_cfg->power.setScanActive;
  if (!g_powerReady || (setScanActive && !setScanActive(true))) {
    g_baselineKnown = false;
    ESP_LOGE("EpdiyLcd", "Panel power-on failed; frame skipped");
    epd_poweroff();
    return false;
  }
  if (!g_baselineKnown) {
    epd_clear();
    memset(g_hl.back_fb, 0xFF, static_cast<size_t>(epd_width()) / 2 * epd_height());
    mode = EpdiyLcdRefresh::Full;
  }
  const int temperature = static_cast<int>(panelTemperature());
  // 文字转页表只在这次推送期间挂上，推完还原，避免影响后续档位的历史与差分基准。
  // / The text-turn table is attached only for this push; it is swapped back afterwards
  // so later profiles keep the waveform the rest of the code expects.
  const EpdWaveform* waveform = waveformFor(mode);
  const bool swapWaveform = waveform != g_hl.waveform;
  if (swapWaveform) epd_hl_waveform(&g_hl, waveform);
  // Full 档要走"所有行列都脏"的整帧路径；其余档位按差分推送——只有差分推送配合
  // 全保持的对角线，未变化的像素才真的不被驱动。
  // / Full takes the all-lines-and-columns-dirty path; every other profile is pushed
  // differentially, and it is only a differential push that lets the held diagonal
  // actually skip a pixel.
  const auto err = mode == EpdiyLcdRefresh::Full ? epd_hl_update_screen_full(&g_hl, drawModeFor(mode), temperature)
                                                 : epd_hl_update_screen(&g_hl, drawModeFor(mode), temperature);
  if (swapWaveform) epd_hl_waveform(&g_hl, &E0470_WAVEFORM);
#if FREEINK_READPICO_DIAGNOSTICS
  int diffMs, drawMs, copyMs;
  epd_hl_last_timing(&diffMs, &drawMs, &copyMs);
  ++g_frameTiming.frames;
  // 走 ROM 控制台，而不是 ESP_LOGI：这个构建里 ESP_LOG 不到串口，板级日志用
  // esp_rom_vprintf 才出得来。诊断只在开发构建里开。
  // / Print through the ROM console rather than ESP_LOGI: ESP_LOG does not reach the
  // serial port in this build, while the board libraries' esp_rom_vprintf does. Only
  // compiled in development builds.
  esp_rom_printf("[EPDF] frame #%u mode=%u result=%u convert=%uus max_convert=%uus diff=%dms scan=%dms copy=%dms\r\n",
                 static_cast<unsigned>(g_frameTiming.frames), static_cast<unsigned>(mode),
                 static_cast<unsigned>(err), static_cast<unsigned>(g_frameTiming.convertUs),
                 static_cast<unsigned>(g_frameTiming.maxConvertUs), diffMs, drawMs, copyMs);
#endif
  const bool biasOff = !setScanActive || setScanActive(false);
  g_baselineKnown = err == EPD_DRAW_SUCCESS && biasOff;
  if (!biasOff) ESP_LOGE("EpdiyLcd", "Panel bias did not turn off");
  if (err != EPD_DRAW_SUCCESS) ESP_LOGE("EpdiyLcd", "Frame failed (%u); clean retry required", static_cast<unsigned>(err));
  if (turnOff || !g_baselineKnown) epd_poweroff();
  return g_baselineKnown;
}

}  // namespace

uint8_t* epdiyLcdBeginGrayscale16() {
  if (!epdiyLcdReady() || g_nativeGrayActive) return nullptr;
  g_nativeGrayActive = true;
  memset(g_fb4, 0xFF, static_cast<size_t>(epd_width()) / 2 * epd_height());
  return g_fb4;
}

bool epdiyLcdCommitGrayscale16(const uint8_t* bwProxy) {
  if (!g_nativeGrayActive || !epdiyLcdReady() || bwProxy == nullptr) return false;
  g_nativeGrayActive = false;
  if (!pushFrame(EpdiyLcdRefresh::Half, true)) return false;
  epdiyLcdStashBase(bwProxy);
  return true;
}

void epdiyLcdCancelGrayscale16() {
  if (!g_nativeGrayActive) return;
  g_nativeGrayActive = false;
  if (epdiyLcdReady() && g_baselineKnown) {
    memcpy(g_fb4, g_hl.back_fb, static_cast<size_t>(epd_width()) / 2 * epd_height());
  }
}

bool epdiyLcdDraw(const uint8_t* fb, EpdiyLcdRefresh mode, bool turnOff) {
  if (!g_started || fb == nullptr || g_fb4 == nullptr || g_cfg == nullptr) return false;

#if FREEINK_READPICO_DIAGNOSTICS
  const int64_t conversionStartedUs = esp_timer_get_time();
#endif
  // 留一份底图：AA 的 displayGray() 提交时调用方的缓冲已经变成选择平面了。
  // / Keep a base copy: by the time the AA displayGray() commit runs, the caller's
  // buffer has become a selector plane.
  const size_t bytes = static_cast<size_t>(epd_width()) / 8 * static_cast<size_t>(epd_height());
  if (g_base != nullptr) memcpy(g_base, fb, bytes);

  fillFrom1bpp(fb);
#if FREEINK_READPICO_DIAGNOSTICS
  recordConversion(conversionStartedUs);
#endif
  return pushFrame(mode, turnOff);
}

void epdiyLcdStashBase(const uint8_t* fb) {
  if (!g_started || fb == nullptr || g_base == nullptr || g_cfg == nullptr) return;
  // 只留底图：不展开、不推帧。灰阶提交会用它合成整页，所以整帧只被一种波形驱动一次。
  // / Stash only: no expansion, no frame push. The grey commit composes the whole page
  // from it, so the frame is driven once by a single profile.
  const size_t bytes = static_cast<size_t>(epd_width()) / 8 * static_cast<size_t>(epd_height());
  memcpy(g_base, fb, bytes);
}

bool epdiyLcdDrawGray(const uint8_t* lsb, const uint8_t* msb, EpdiyLcdRefresh mode, bool turnOff) {
  if (!g_started || g_fb4 == nullptr || g_cfg == nullptr) return false;
  if (g_base == nullptr || lsb == nullptr || msb == nullptr) return false;

#if FREEINK_READPICO_DIAGNOSTICS
  const int64_t conversionStartedUs = esp_timer_get_time();
#endif
  // The existing 2-bit coverage maps directly to four panel tones. A set base
  // bit is white (15); selector masks choose the two calibrated mid tones.
  constexpr uint8_t kDarkGray = 3;   // 2-bit 值 1（深灰）/ 2-bit value 1 (dark)
  constexpr uint8_t kLightGray = 8;  // 2-bit 值 2（浅灰）/ 2-bit value 2 (light)
  constexpr int kInkLevel = 0;       // 墨 = 黑 / ink is black
  constexpr int kPaperLevel = 15;    // 纸 = 白 / paper is white

  const int w = static_cast<int>(epd_width());
  const int h = static_cast<int>(epd_height());
  const int stride = w / 8;

  for (int y = 0; y < h; ++y) {
    const uint8_t* lrow = lsb + static_cast<size_t>(y) * stride;
    const uint8_t* mrow = msb + static_cast<size_t>(y) * stride;
    uint8_t* drow = g_fb4 + static_cast<size_t>(y) * (w / 2);

    const auto toneFor = [](bool baseInk, bool lb, bool mb) -> uint8_t {
      if (mb && !lb) return kLightGray;
      if (lb) return kDarkGray;
      return baseInk ? kInkLevel : kPaperLevel;
    };
    for (int x = 0; x < w; x += 2) {
      const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
      const uint8_t base = g_base[static_cast<size_t>(y) * stride + (x >> 3)];
      const uint8_t l = lrow[x >> 3], m = mrow[x >> 3];
      const uint8_t low = toneFor((base & mask) == 0, (l & mask) != 0, (m & mask) != 0);
      const uint8_t high = toneFor((base & (mask >> 1)) == 0, (l & (mask >> 1)) != 0, (m & (mask >> 1)) != 0);
      // Both nibbles are known: one PSRAM store, without reading the old byte.
      // 这个逐字节的写法实测比「按源字节拼 32 位字」的写法快 66%，不要再改回去。
      // / Both nibbles are known: one store, without reading the old byte. Measured 66%
      // faster than assembling a 32-bit word per source byte; do not "optimize" it back.
      drow[x >> 1] = static_cast<uint8_t>(low | (high << 4));
    }
  }

#if FREEINK_READPICO_DIAGNOSTICS
  recordConversion(conversionStartedUs);
#endif
  return pushFrame(mode, turnOff);
}

void epdiyLcdDeepSleep() {
  if (!g_started) return;
  (void)epd_poweroff();
  epdiyLcdEnd();
}

bool epdiyLcdReady() { return g_started && g_fb4 != nullptr; }

}  // namespace freeink
