#!/bin/sh
# Builds and runs the BQ27220 Design Capacity load against a model gauge, the
# SGM41562 charger decode (Picco) against a model charger, the CW2015 gauge
# (EEGO A4) setup/read and ADC fallback against a model gauge, and the usbDetect
# external-power path (PaperS3: no charger IC and no gauge, so the profile's
# detect pin is the only source for "is a cable attached"; X3 must not read SDA).
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-battery-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Werror \
  -DFREEINK_DEVICE_X3=1 -DFREEINK_DEVICE_X4=1 -DFREEINK_BATTERY_I2C_GAUGE=1 \
  -Istubs -I../../include -I../../../BoardConfig/include -I../../../XteinkDetect/test/host/stubs \
  test_bq27220_capacity.cpp -o "$BUILD_DIR/test_bq27220_capacity"
"$BUILD_DIR/test_bq27220_capacity"
c++ -std=c++17 -Wall -Wextra -Werror \
  -DFREEINK_DEVICE_PICCO=1 \
  -Istubs -I../../include -I../../../BoardConfig/include -I../../../XteinkDetect/test/host/stubs \
  test_sgm41562.cpp -o "$BUILD_DIR/test_sgm41562"
"$BUILD_DIR/test_sgm41562"
c++ -std=c++17 -Wall -Wextra -Werror \
  -DFREEINK_DEVICE_EEGO_A4=1 \
  -Istubs -I../../include -I../../../BoardConfig/include -I../../../XteinkDetect/test/host/stubs \
  test_cw2015.cpp -o "$BUILD_DIR/test_cw2015"
"$BUILD_DIR/test_cw2015"
c++ -std=c++17 -Wall -Wextra -Werror \
  -DFREEINK_DEVICE_PAPERS3=1 -DFREEINK_BATTERY_I2C_GAUGE=1 \
  -Istubs -I../../include -I../../../BoardConfig/include -I../../../XteinkDetect/test/host/stubs \
  test_adc_usb_detect.cpp ../../src/BatteryMonitor.cpp -o "$BUILD_DIR/test_adc_usb_detect"
"$BUILD_DIR/test_adc_usb_detect"

c++ -std=c++17 -Wall -Wextra -Werror -I../../include \
  test_estimate.cpp -o "$BUILD_DIR/test_estimate"
"$BUILD_DIR/test_estimate"
