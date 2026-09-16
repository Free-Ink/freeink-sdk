#!/bin/sh
set -eu
cd "$(dirname "$0")"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/freeink-metalio-audio.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-private-field -DFREEINK_DEVICE_METALIO_EINK4=1 \
  -Istubs -I../../../InputManager/test/host/metalio_stubs \
  -I../../../BoardConfig/include test_metalio_audio.cpp -o "$BUILD_DIR/test"
"$BUILD_DIR/test"
c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-private-field -DFREEINK_DEVICE_METALIO_EINK4=1 \
  -Istubs -I../../../InputManager/test/host/metalio_stubs -I../../../BoardConfig/include \
  -I../../include -I../../../Microphone/include test_metalio_managers.cpp \
  ../../src/AudioManager.cpp ../../../Microphone/src/Microphone.cpp -o "$BUILD_DIR/managers"
"$BUILD_DIR/managers"
for flags in '-DFREEINK_DEVICE_METALIO_EINK4=1 -DFREEINK_CAP_AUDIO=0 -DFREEINK_CAP_MIC=0' '-DFREEINK_DEVICE_X4=1'; do
  c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-private-field $flags \
    -Istubs -I../../../InputManager/test/host/metalio_stubs -I../../../BoardConfig/include \
    -I../../include -I../../../Microphone/include test_absent.cpp \
    ../../src/AudioManager.cpp ../../../Microphone/src/Microphone.cpp -o "$BUILD_DIR/absent"
  "$BUILD_DIR/absent"
done
