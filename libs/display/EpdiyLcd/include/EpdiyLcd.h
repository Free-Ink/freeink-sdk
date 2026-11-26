/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Board glue for the vendor epdiy LCD fork (LGPL-3.0-or-later).
// Local highlevel changes retain the baseline on draw failure, check allocations,
// and release owned buffers; see test/host/test_transactions.py.
// Linked only on raw parallel targets. Panel VCOM remains the factory PMU value;
// the epdiy set_vcom callback never writes it.

#include <Arduino.h>

namespace freeink {

/// 板级电源钩子。与 LgfxEpdPowerHooks 同形，但定义在这里，避免本库反向依赖
/// FreeInkDisplay（那会成环）。任一可为 nullptr。
/// / Board power hooks. Same shape as LgfxEpdPowerHooks but defined here so this
/// library does not depend back on FreeInkDisplay. powerOn must report successful rail bring-up.
struct EpdiyLcdPowerHooks {
  bool (*prepare)();   ///< 上电前：引脚归安全电平，轨全部关。/ Park pins, rails down.
  bool (*powerOn)();   ///< VCOM 校验通过后才升轨。/ Raise rails after the VCOM check.
  void (*powerOff)();  ///< 掉轨。/ Drop rails.
  /// 面板温度，用来选波形的温度档。nullptr 或返回值不在 0..50 时按 20 °C 处理。
  /// 本板波形只有一个 0–50 °C 档，所以这个值不改变选表结果。
  /// / Panel temperature, used to pick the waveform's temp range. nullptr, or a
  /// value outside 0..50, falls back to 20 °C — this board's table has a single
  /// 0–50 °C range, so the value does not change which phases run.
  float (*getTemperature)();
  // Enable the panel bias before scans and disable it between refreshes.
  bool (*setScanActive)(bool active) = nullptr;
};

/// 一行四段 + CKV，单位是像素钟个数（ckv 是 0.1µs）。对应 epdiy 的
/// LcdLineTiming_t。行长 = leHighTime + lineFrontPorch + lineData + lineEnd，
/// lineData 由面板宽度和总线宽度推出，不在配置里。
/// / One scan line in four segments plus CKV, in pixel clocks (CKV in 0.1 µs).
/// `lineData` is derived from the panel width / bus width, so it is not here.
struct EpdiyLcdLineTiming {
  int leHighTime;
  int lineFrontPorch;
  int lineEnd;
  int ckvHighTime01us;
};

/// 面板总线接线与扫描参数。几何尺寸不在这里，来自 ACTIVE BoardProfile。
/// / Panel bus wiring and scan parameters. Geometry is not here; it comes from
/// the ACTIVE BoardProfile, like every other driver.
struct EpdiyLcdConfig {
  int8_t dataPins[16];   ///< D0..D15。-1 = 未接（BoardConfig::PIN_UNASSIGNED）。
  int8_t pinClock;       ///< XCL，外部像素钟（LCD_CAM pclk 输出）。
  int8_t pinCkv;         ///< CKV，栅极钟（RMT 产生）。
  int8_t pinStartPulse;  ///< XSTL，接 LCD 的 DE 信号。
  int8_t pinLeh;         ///< XLE，接 LCD 的 HSYNC 信号。
  int8_t pinStv;         ///< SPV，帧起始脉冲。
  uint8_t busWidth;      ///< 8 或 16。
  int pclkMhz;           ///< 像素钟 MHz（本板 18）。
  EpdiyLcdLineTiming line;
  uint8_t prefillLines;  ///< 每相开扫前的预填行数，决定帧间隙。
  EpdiyLcdPowerHooks power;
};

/// 刷新档位，映射到 epdiy 的 MODE_*。/ Refresh profile, mapped onto epdiy MODE_*.
///
/// TextTurn 与 Half 同为 GL16，但用 E0470_TEXTTURN_WAVEFORM：对角线全保持，未变化的
/// 像素完全不驱动。原地重推一个黑像素会先擦白再推黑，那正是翻页可见的白闪；抗锯齿
/// 文字页的常规翻页用它，静止内容的刷新交给周期性 GC16。
/// / TextTurn is GL16 like Half but drives E0470_TEXTTURN_WAVEFORM, whose diagonal is
/// entirely held so unchanged pixels are not driven at all. Re-driving a black pixel in
/// place erases it white first, which is the white flash a turn shows; ordinary
/// anti-aliased text turns use it, and static content is refreshed by the periodic GC16.
enum class EpdiyLcdRefresh : uint8_t { Full, Half, Fast, TextTurn };

/// 初始化总线并挂上波形。width/height 是面板扫描尺寸（本板 1216x684）。
///
/// `blackIsOne` 在这里给定一次，说明调用方的 1bpp 帧缓冲里 1 代表黑还是白；内部
/// 需要同一个约定去建开机白场，所以不再每次推帧重复传。
///
/// 失败返回 false（例如 PSRAM 里的 4bpp 帧缓冲分配不出来）。
/// / Bring up the bus and attach the waveform. Returns false on failure (e.g. the
/// 4 bpp framebuffer could not be allocated in PSRAM). `blackIsOne` fixes the
/// caller's 1 bpp bit convention once, because the internal boot-time white field
/// must be built with that same convention.
bool epdiyLcdBegin(const EpdiyLcdConfig& cfg, uint16_t width, uint16_t height, bool blackIsOne);

/// 释放总线与帧缓冲。/ Release the bus and the framebuffer.
void epdiyLcdEnd();

// Borrow the existing front framebuffer. No allocation. Commit uses GL16 and
// promotes an unknown baseline to GC16; cancellation never changes the glass.
uint8_t* epdiyLcdBeginGrayscale16();
bool epdiyLcdCommitGrayscale16(const uint8_t* bwProxy);
void epdiyLcdCancelGrayscale16();

/// 推一帧。`fb` 是 1bpp、MSB 在前、每行 width/8 字节、height 行；位约定由
/// epdiyLcdBegin 的 `blackIsOne` 给定。
/// / Push one frame. `fb` is 1 bpp MSB-first, width/8 bytes per row, height rows;
/// the bit convention is the one handed to epdiyLcdBegin.
bool epdiyLcdDraw(const uint8_t* fb, EpdiyLcdRefresh mode, bool turnOff);

/// 只把这一页留作底图，不推屏。给「底图与灰阶合并成一次波形」的宿主用：宿主随后调用
/// epdiyLcdDrawGray()，由它用这张底图合成整页并只推一次。
/// / Stash this page as the base WITHOUT presenting it. For hosts that combine the
/// base and the grey planes into one waveform: they then call epdiyLcdDrawGray(),
/// which composes the whole page from this base and presents it once.
void epdiyLcdStashBase(const uint8_t* fb);

/// 推一帧中间灰：以最近一次 epdiyLcdDraw() 的黑白页为底，再用 LSB/MSB 选择平面对
/// 被选中的像素做中间灰覆盖。
///
/// 注意 `fb` 不在这里：anti-aliasing 提交时，调用方手里那个缓冲装的是**最后写入的
/// 选择平面**，不是页面（平面背景 0、灰标记 1，与页面互补），把它当页面推上去得到
/// 的是负片。LgfxEpdDriver.cpp:187-201 为同一个坑留过注释，这里照它的结论保存底图。
///
/// English: Push a mid-gray frame: start from the B/W page of the last
/// epdiyLcdDraw() and overlay the pixels the LSB/MSB selector planes pick. `fb` is
/// deliberately absent — at anti-aliasing commit time the caller's buffer holds the
/// LAST SELECTOR PLANE, not the page (plane background 0, gray marks 1, i.e. the
/// complement of the page), so pushing it yields a negative. LgfxEpdDriver.cpp:187-201
/// documents the same trap; this keeps a base image for the same reason.
bool epdiyLcdDrawGray(const uint8_t* lsb, const uint8_t* msb, EpdiyLcdRefresh mode, bool turnOff);

/// 进入低功耗：掉轨并释放 LCD_CAM/GDMA/RMT。
/// / Enter low power: drop the rails and release LCD_CAM/GDMA/RMT.
void epdiyLcdDeepSleep();

/// 上一帧是否真的推到了面板（调试/对账用）。
/// / Whether the last frame actually reached the panel (diagnostics).
bool epdiyLcdReady();

}  // namespace freeink
