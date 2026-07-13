# Deferred refresh: one split interface (migration notes)

This change collapses the two parallel deferred-refresh mechanisms —
`PanelDriver::displayAsync()` (SSD1677/X4) and `displayStart()/displayFinish()`
(UC8253-X3) — into the single split interface, and merges the facade's two
pending flags into one. It builds on the `crosspoint-async-display-split` and
`x4-async-trigger-split` work; nothing conceptual is new, there is just one
mechanism now instead of two.

## Why

- `displayAsync` and `displayStart/displayFinish` expressed the same idea
  ("fire the waveform, come back later") with different plumbing, doubling the
  facade states (`_asyncPending` + `_splitPending`), the finish paths
  (`finishDisplayAsync` vs `completeDisplay` vs plain `waitBusy`), and the
  capability signals (`supportsAsyncDisplay()` vs displayStart's return value).
- The `waitBusy("async refresh")` drain for the X4 async path bypassed the ISR
  edge wait and, on drivers that fell back to blocking `display()`, spun the
  X3TwoPhase edge-detect timeout (1 s) against an already-idle panel.

## Driver interface (PanelDriver)

- **`displayAsync()` is removed.** The split is the one deferred mechanism:

  ```cpp
  // fire the waveform; true = deferred (displayFinish() must run), false = completed inline
  virtual bool displayStart(EpdBus&, const uint8_t* fb, const uint8_t* prev, RefreshMode, bool turnOff);
  // wait out the waveform + any post-waveform pipeline (X3: DTM1 sync + conditioning)
  virtual void displayFinish(EpdBus&, const uint8_t* fb);
  ```

  Defaults are unchanged (blocking `display()`, return false / no-op), so
  drivers without deferral need no edits.

- **`supportsAsyncDisplay()`** now means "displayStart() defers". It must agree
  with what `displayStart()` returns. It exists so the facade can skip shadow
  setup without a trial call, and so hosts can size overlap buffers up front.
  Overrides: SSD1677 → true, UC8253-X3 → true (new — this is what lets hosts
  run the same overlap flow on both panels).

- **SSD1677** gains the split: `displayStart()` is the old `displayAsync` body
  (displayImpl async path, returns true), `displayFinish()` is
  `bus.waitRefreshComplete("refresh")` (ISR edge wait; the done-level fast path
  handles a waveform that finished before the host got around to waiting).

## Facade (FreeInkDisplay)

- **One pending flag** (`_refreshPending`) replaces `_asyncPending` +
  `_splitPending`. It is set from `displayStart()`'s return value and drained
  in exactly one place (`syncPendingAsync()`), which always finishes through
  `driver->displayFinish()` — so the X3 post-waveform pipeline can never be
  skipped by a plain busy-wait again, and the X4 wait is always the ISR path.
- **Entry points are unchanged** — all of these still exist and now delegate to
  the same `displayAsyncImpl(mode, turnOff, noShadow)`:
  - `displayBufferAsync(mode)` — shadowed async (single-buffer allocates the
    48 KB shadow; caller may redraw fb immediately).
  - `displayBufferAsyncNoShadow(mode)` — no shadow; caller keeps fb intact
    until the wait and re-seeds the baseline itself (the tiled-AA flow).
  - `triggerDisplay(mode, off)` / `triggerDisplayAsync(mode, off)` — CrossPoint
    compat names, same semantics as before.
- **Finish/wait are aliases of one function**: `waitRefreshComplete()`,
  `completeDisplay()`, and `finishDisplayAsync()` all drain the single pending
  state. Call whichever your codebase already uses.
- `refreshBusy()` no longer clears the pending state when the pin reads idle —
  the driver's post-waveform work must still run. When it returns false, call
  any of the finish aliases (or any blocking display op, which self-heals).
- One behavior fix: single-buffer `displayBufferAsync()` on the **X3** now
  takes the blocking path instead of the shadow path. The shadow contract
  ("redraw fb immediately") is unsound on X3 because `displayFinish()` re-reads
  fb for the DTM1 sync; use `displayBufferAsyncNoShadow()` or
  `triggerDisplay()` (frame-intact contracts) for X3 overlap.

## What Witch Reader needs to change

Almost nothing at the call sites — the compat surface is intact. Checklist:

1. If any code overrides or calls `PanelDriver::displayAsync()` directly,
   port it to `displayStart()`/`displayFinish()` (see the SSD1677 diff for the
   1:1 mapping).
2. If any code assumed `refreshBusy() == false` implies "fully done", add a
   `waitRefreshComplete()` (cheap no-op when nothing is pending).
3. Custom deferring drivers must override `supportsAsyncDisplay()` to true.

## X4 fast-DU shortcut is now opt-in

`fastDuRefreshShortcut` (CTRL2=0x1C instead of the stock 0xFC partial
sequence) is no longer applied to the X4 by default — it is gated behind
`-DFREEINK_X4_FAST_DU_SHORTCUT`. The 0x1C path skips the per-refresh
temperature load and power sequencing; that is the community-sdk behavior the
stock-parity work moved away from after ghosting/blotching reports on some
panels, and its artifacts tend to appear only over long sessions and across
temperature. Worth ~85 ms/refresh where a panel is validated; enable the flag
per build once you have that validation, the mechanism is unchanged.

## What this buys

Measured on X4 (800x480, AA on, 16 MHz SPI): page turn 1274 ms → 822 ms by
overlapping the grayscale plane rendering with the BW waveform via the
no-shadow split. With X3 now reporting deferral through the same interface,
the identical host-side flow overlaps on X3 with no host changes.
