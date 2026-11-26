/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：Read Pico 的 epdiy LCD 路径板级配置——总线接线、面板扫描时序、电源钩子。
 * 时序算法照搬厂商 components/read_pico/read_pico_epd_timing.c（Apache-2.0），
 * 只把出口从 epd_lcd_init 换成 freeink::EpdiyLcdConfig。
 *
 * English: Read Pico board config for the epdiy LCD path — bus wiring, panel scan
 * timing, power hooks. The timing math is carried over from the vendor's
 * components/read_pico/read_pico_epd_timing.c (Apache-2.0) with the exit point
 * changed from epd_lcd_init to freeink::EpdiyLcdConfig.
 *
 * 冻结 / Frozen：
 *   - VCOM 只读。这里只登记钩子，绝不产生 VCOM 写。
 *     / VCOM is read-only. This file only registers hooks and never writes VCOM.
 *   - 引脚号以 read_pico_board.c 为准，不得为了方便改这里。
 *     / Pin numbers follow read_pico_board.c; do not "tidy" them here.
 */

#include <BoardConfig.h>
#include <EpdiyLcd.h>

#include "BoardReadPico.h"
#include "BoardReadPicoPins.h"

namespace freeink {
namespace {

// --- 扫描时序解算 / Scan-timing solve ---------------------------------------
// 规格消隐下界 / Spec blanking floors.
constexpr int kLineStartMinNs = 300;
constexpr int kLineBackMinNs = 800;
constexpr int kLineEndMinClk = 4;
// 帧消隐：起始 1 行、后肩 4 行、结束 12 行。/ Frame blanking: 1 / 4 / 12 lines.
constexpr int kFrameStartLines = 1;
constexpr int kFrameBackLines = 4;
constexpr int kFrameEndLines = 12;

// 栅极开门短于一行：厂家 LGONL≈13.4µs，低电平至少留 1.2µs。
// / Gate open shorter than a line: vendor LGONL≈13.4 µs, ≥1.2 µs low.
constexpr int kCkvLowMin01us = 12;
constexpr int kCkvHighMax01us = 134;

// 每像素 2bit，一个像素钟推满整条总线 / 2 bits per pixel, one clock fills the bus.
constexpr int kBusWidth = 16;
constexpr int kPxPerClk = kBusWidth / 2;

constexpr int ceilDiv(int n, int d) { return (n + d - 1) / d; }

// 一段最小时间换成 f MHz 下的整数钟 / minimum time to integer clocks at f MHz.
constexpr int clocksForNs(int ns, int pclkMhz) { return ceilDiv(ns * pclkMhz, 1000); }

constexpr int clampInt(int v, int lo, int hi) { return v > hi ? hi : (v < lo ? lo : v); }

// FULL 档的行周期要凑到波形标定的帧周期，驱动量正比于它。
// / FULL pads the line period to the waveform's frame-period setpoint; drive
// scales with it.
constexpr int kFrameTargetUs = 11090;  // E0470_WAVEFORM_FRAME_US

struct Scan {
  int pclkMhz;
  int lineStart;
  int lineBackPorch;
  int lineEnd;
  int ckvHigh01us;
};

// read_pico_epd_scan() 的 FULL 档，去掉 profile/epd 依赖后的定值求解。
// / read_pico_epd_scan() for the FULL profile, with the profile/epd dependencies
// folded out. Only the FULL profile is used here: the fast profiles exist for the
// vendor's follow/continuous DU, which this port does not drive.
constexpr Scan solveScan(int pclkMhz, int width, int height) {
  const int ldl = width / kPxPerClk;
  const int vAll = kFrameStartLines + kFrameBackLines + height + kFrameEndLines;
  const int lsl = clocksForNs(kLineStartMinNs, pclkMhz);
  const int lbl = clocksForNs(kLineBackMinNs, pclkMhz);

  // 余量给 L_EL，让 L_DL 落在 CKV 高电平里；后肩只取规格下界。
  // / Slack goes to L_EL so L_DL sits inside CKV high; back porch is the floor.
  int lineUs = ceilDiv(ldl + lsl + lbl + kLineEndMinClk, pclkMhz);
  const int targetLineUs = (kFrameTargetUs + vAll / 2) / vAll;
  if (targetLineUs > lineUs) lineUs = targetLineUs;

  int lineClocks = lineUs * pclkMhz;
  int lel = lineClocks - ldl - lsl - lbl;
  if (lel < kLineEndMinClk) {
    ++lineUs;
    lineClocks = lineUs * pclkMhz;
    lel = lineClocks - ldl - lsl - lbl;
  }

  // 开门至少盖住有效数据，再按厂家 LGONL 上限收；短行盖不住就收到行长减 1.2µs。
  // / Gate must cover active data, then cap at vendor LGONL; if the line is too
  // short to fit, cap at line minus 1.2 µs.
  const int dataEnd01us = ceilDiv((lsl + lbl + ldl) * 10, pclkMhz);
  int ckv = kCkvHighMax01us;
  if (ckv < dataEnd01us) ckv = dataEnd01us;
  const int ckvHigh = clampInt(ckv, kCkvLowMin01us, lineUs * 10 - kCkvLowMin01us);

  return Scan{pclkMhz, lsl, lbl, lel, ckvHigh};
}

constexpr int kPanelWidth = 1216;
constexpr int kPanelHeight = 684;
constexpr int kPclkMhz = READPICO_PCLK_HZ / 1000000;
constexpr Scan kScan = solveScan(kPclkMhz, kPanelWidth, kPanelHeight);

// FULL 档的预填行数：一行 16µs，喂线程余量大，32 行实测无欠载，帧间隙归零。
// / FULL-profile prefill: 16 µs/line leaves slack, 32 lines measured underrun-free
// and closes the frame gap.
constexpr uint8_t kPrefillLines = 32;

}  // namespace

const EpdiyLcdConfig& readPicoEpdiyConfig() {
  // 成员按 EpdiyLcdConfig 的声明顺序（positional）。18 MHz 下解出：
  // L_SL=6, L_BL=15, L_DL=152, L_EL=115, CKV=134 (0.1µs)。
  // / Members are positional in EpdiyLcdConfig declaration order. At 18 MHz this
  // solves to L_SL=6, L_BL=15, L_DL=152, L_EL=115, CKV=134 (0.1 µs).
  static const EpdiyLcdConfig cfg = {
      {READPICO_EP_D0, READPICO_EP_D1, READPICO_EP_D2, READPICO_EP_D3, READPICO_EP_D4, READPICO_EP_D5, READPICO_EP_D6,
       READPICO_EP_D7, READPICO_EP_D8, READPICO_EP_D9, READPICO_EP_D10, READPICO_EP_D11, READPICO_EP_D12,
       READPICO_EP_D13, READPICO_EP_D14, READPICO_EP_D15},
      READPICO_EP_XCL,   // pinClock     -> LCD_CAM pclk 输出 / pclk out
      READPICO_EP_CKV,   // pinCkv       -> RMT 产生的栅极钟 / gate clock from RMT
      READPICO_EP_XSTL,  // pinStartPulse-> LCD 的 DE（参考固件 board_poweron 的同一根线）
                         //                / the LCD's DE, same line as the reference
      READPICO_EP_XLE,   // pinLeh       -> LCD 的 HSYNC / the LCD's HSYNC
      READPICO_EP_SPV,   // pinStv       -> 帧起始脉冲 / frame start pulse
      kBusWidth,
      kPclkMhz,
      {kScan.lineStart, kScan.lineBackPorch, kScan.lineEnd, kScan.ckvHigh01us},
      kPrefillLines,
      // 电源序列复用 板级电源钩子（ReadPicoPower.cpp）：它们已经是
      // §1.4 验证过的 board_poweron() 顺序，且 VCOM 门在升任何轨之前。
      // / Reuse the board power hooks (ReadPicoPower.cpp): they already
      // implement the verified board_poweron() order with the VCOM gate before any
      // rail comes up.
      {&BoardReadPico::epdPrepare, &BoardReadPico::epdPowerOn, &BoardReadPico::epdPowerOff, nullptr,
       &BoardReadPico::epdSetScanActive},
  };
  return cfg;
}

}  // namespace freeink
