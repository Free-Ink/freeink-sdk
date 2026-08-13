#pragma once

// M5Stack PaperS3 pin map — the parallel-EPD bus and the board-support-only
// pins that don't live in the BoardProfile (SD/touch/RTC/battery/buzzer pins
// are in BoardConfig::M5PAPER_S3).
//
// Sources: M5GFX autodetect (board_M5PaperS3 in src/M5GFX.cpp) cross-checked
// against M5Unified and the official docs pinmap. Where the docs table
// disagrees (it labels GPIO45 "PWR" and omits GPIO16/GPIO46), M5GFX — the
// working vendor driver — is authoritative: OE=45, PWR=46, CL=16.

// ED047TC1 8-bit parallel data bus, D0..D7.
#define PAPERS3_EP_D0 6
#define PAPERS3_EP_D1 14
#define PAPERS3_EP_D2 7
#define PAPERS3_EP_D3 12
#define PAPERS3_EP_D4 9
#define PAPERS3_EP_D5 11
#define PAPERS3_EP_D6 8
#define PAPERS3_EP_D7 10

// Row/frame timing + power. Bus_EPD's stock power sequence drives OE/PWR/SPV
// itself (on: OE, PWR, SPV with settling delays; off in reverse) — no PMIC.
#define PAPERS3_EP_SPH 13  // XSTL: start pulse, horizontal
#define PAPERS3_EP_CL 16   // XCL: pixel clock
#define PAPERS3_EP_LE 15   // XLE: latch enable
#define PAPERS3_EP_SPV 17  // start pulse, vertical
#define PAPERS3_EP_CKV 18  // vertical clock
#define PAPERS3_EP_OE 45   // output enable
#define PAPERS3_EP_PWR 46  // EPD rail enable

// Power-off pulse to the PMS150G latch controller. The PMS150G latches system
// power on its own (no hold pin for firmware); driving this pin through a
// low/high pulse train releases the latch. Idles LOW.
#define PAPERS3_PWROFF_PULSE 44

// Single PWM status LED (active-high). Not an addressable strip, so it is not
// in the profile's LedConfig — drive it with LEDC/digitalWrite as needed.
#define PAPERS3_LED 0
