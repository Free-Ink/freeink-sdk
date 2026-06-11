#pragma once
#include <cstdint>

// FreeInk SDK — battery monitor.
//
// Two backends behind one API (chosen at compile time, so construction and the
// public methods below are identical for both):
//   * ADC (default) — reads a divided LiPo voltage off an ADC pin and maps it to
//     a percentage; an optional charge-status pin (MCP73832 /STAT, active-LOW)
//     drives isCharging().
//   * I2C fuel gauge (FREEINK_BATTERY_I2C_GAUGE) — reads SoC/voltage/charge from a
//     BQ27220 gauge (+ optional BQ25896 charger) over I2C; the ADC pin/divider are
//     ignored. Used by X3 and LilyGo T5 S3. Config comes from
//     BoardConfig::ACTIVE.batteryGauge, and gauge-vs-ADC is chosen at *runtime*
//     (gaugeAddr != 0), so X3 (gauge) and X4 (ADC) work from one C3 binary.
class BatteryMonitor {
public:
    static constexpr int8_t PIN_NONE = -1;

    struct Status {
        bool supported = false;
        bool percentageKnown = false;
        bool millivoltsKnown = false;
        bool chargingKnown = false;
        bool externalPowerKnown = false;
        uint16_t percentage = 0;
        uint16_t millivolts = 0;
        bool charging = false;
        bool externalPower = false;
        // Raw M5PM1 telemetry for diagnostics; -1 when not read (non-PM1 boards
        // or failed I/O). VIN is the DC input rail, 5VINOUT the bidirectional
        // USB-C rail, powerSource the PM1's PWR_SRC report (reg 0x04 [2:0]).
        int32_t pm1VinMv = -1;
        int32_t pm1VinOutMv = -1;
        int16_t pm1PowerSource = -1;
    };

    // Uses BoardConfig::ACTIVE's battery pins/backend.
    BatteryMonitor();

    // Optional divider multiplier defaults to 2.0; optional charge-status pin
    // defaults to unused.
    explicit BatteryMonitor(int8_t adcPin, float dividerMultiplier = 2.0f, int8_t chargeStatusPin = PIN_NONE);

    // Read voltage and return percentage (0-100)
    uint16_t readPercentage() const;

    // Like readPercentage(), but reports whether the read succeeded. An I2C gauge
    // can fail transiently; on failure this returns false and leaves `out`
    // unchanged, so a caller can keep its last good value. The ADC path always
    // succeeds.
    bool readPercentageChecked(uint16_t& out) const;

    // Read every battery field the active board can report. `supported` is false
    // when the board profile has no battery telemetry path. Per-field `Known`
    // flags distinguish a valid false/zero value from unsupported or failed I/O.
    Status readStatus() const;

    // Read the battery voltage in millivolts (accounts for divider)
    uint16_t readMillivolts() const;

    // Read the battery voltage in volts (accounts for divider)
    double readVolts() const;

    // True when the charger reports an active charge (charge-status pin LOW).
    // Always false when no charge-status pin is configured.
    bool isCharging() const;

    // Percentage (0-100) from a millivolt value
    static uint16_t percentageFromMillivolts(uint16_t millivolts);

private:
    bool hasAdcBackend() const;
    bool hasGaugeBackend() const;
    bool hasM5Pm1Backend() const;
    bool readM5Pm1Status(Status& status) const;

    int8_t _adcPin;
    float _dividerMultiplier;
    int8_t _chargeStatusPin;
};
