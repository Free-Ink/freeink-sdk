#!/usr/bin/env python3
"""Compile the real ReadPico driver and verify supported modes and submission lifecycle."""
from pathlib import Path
import os,shutil,subprocess,tempfile
HERE=Path(__file__).resolve().parent
LIB=HERE.parents[1]
with tempfile.TemporaryDirectory(prefix='freeink-epdiy-contract-') as directory:
    root=Path(directory)
    for folder in ['src/driver','src/bus','include']:(root/folder).mkdir(parents=True)
    for name in ['EpdiyLcdDriver.h','EpdiyLcdDriver.cpp','PanelDriver.h']:
        shutil.copy2(LIB/'src/driver'/name,root/'src/driver'/name)
    for name in ['GrayscaleCapabilities.h']:
        shutil.copy2(LIB/'include'/name,root/'include'/name)
    shutil.copy2(HERE/'pro_stubs/EpdBus.h',root/'src/bus/EpdBus.h')
    shutil.copy2(HERE/'pro_stubs/Arduino.h',root/'Arduino.h')
    (root/'BoardConfig.h').write_text('''#pragma once
#include <cstdint>
struct TestSerial { template<class... Args> void printf(const char*,Args...){} };
inline TestSerial Serial;
namespace BoardConfig {
struct Profile { uint16_t displayWidth=800, displayHeight=480; };
inline constexpr Profile ACTIVE;
}
''')
    (root/'esp_heap_caps.h').write_text('''#pragma once
#include <cstdlib>
inline int failAllocation=-1;
#define MALLOC_CAP_SPIRAM 1
inline void* heap_caps_malloc(size_t n,int) {
  if(failAllocation==0)return nullptr;
  if(failAllocation>0)--failAllocation;
  return std::malloc(n);
}
inline void heap_caps_free(void* p){std::free(p);}
''')
    (root/'EpdiyLcd.h').write_text('''#pragma once
#include <cstdint>
#include <cstdlib>
struct EpdiyLcdConfig {};
enum class EpdiyLcdRefresh { Full, Half, Fast, TextTurn };
inline bool ready=false, drawOk=true;
inline int draws=0, grayDraws=0, stashes=0;
inline uint8_t native[1];
inline bool epdiyLcdBegin(const EpdiyLcdConfig&,uint16_t,uint16_t,bool){ready=true;return true;}
inline void epdiyLcdEnd(){ready=false;}
inline void epdiyLcdDeepSleep(){ready=false;}
inline bool epdiyLcdDraw(const uint8_t*,EpdiyLcdRefresh,bool){++draws;return drawOk;}
inline void epdiyLcdStashBase(const uint8_t*){++stashes;}
inline bool epdiyLcdDrawGray(const uint8_t*,const uint8_t*,EpdiyLcdRefresh,bool){++grayDraws;return drawOk;}
inline uint8_t* epdiyLcdBeginGrayscale16(){return ready?native:nullptr;}
inline bool epdiyLcdCommitGrayscale16(const uint8_t*){return ready&&drawOk;}
inline void epdiyLcdCancelGrayscale16(){}
''')
    (root/'test.cpp').write_text('''#include <cassert>
#include <cstdio>
#include <vector>
#include <esp_heap_caps.h>
#include "src/driver/EpdiyLcdDriver.h"
namespace freeink { const EpdiyLcdConfig& testConfig(){static EpdiyLcdConfig c;return c;} }
int main(){
  using namespace freeink;
  EpdBus bus;
  std::vector<uint8_t> bw(48000,0xff), plane(48000,0);
  EpdiyLcdDriver d(testConfig());
  auto c=d.grayscaleCapabilities(GrayscaleMode::Overlay);
  assert(c.supported()&&c.encoding==GrayscaleEncoding::OverlayMasks&&c.base==GrayscaleBase::Combined&&!c.stripUploads);
  assert(!d.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(!d.grayscaleCapabilities(GrayscaleMode::Direct).supported());
  assert(!d.displayCommitted());
  d.begin(bus);
  d.displayGrayscaleBase(bus,bw.data(),RefreshMode::Half,false);
  assert(!d.displayCommitted()&&stashes==1&&draws==0);
  d.copyGrayscaleLsb(bus,plane.data());d.copyGrayscaleMsb(bus,plane.data());
  d.displayGray(bus,bw.data(),false,nullptr,false);
  assert(d.displayCommitted()&&grayDraws==1);
  d.cleanupGrayscaleBuffers(bus,bw.data());assert(draws==0); // successful gray commit is not driven twice
  d.displayGrayscaleBase(bus,bw.data(),RefreshMode::Half,false);
  d.cleanupGrayscaleBuffers(bus,bw.data());assert(d.displayCommitted()&&draws==1); // B/W fallback
  d.displayGrayscaleBase(bus,bw.data(),RefreshMode::Half,false);
  d.requestResync(0);d.cleanupGrayscaleBuffers(bus,bw.data());
  assert(!d.displayCommitted()&&draws==1); // cancelled target is not activated
  drawOk=false;d.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);assert(!d.displayCommitted());
  d.displayGrayscaleBase(bus,bw.data(),RefreshMode::Half,false);
  d.displayGray(bus,bw.data(),false,nullptr,false);assert(!d.displayCommitted());
  d.cleanupGrayscaleBuffers(bus,bw.data());assert(!d.displayCommitted());
  drawOk=true;d.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);assert(d.displayCommitted());
  d.displayGrayscaleBase(bus,bw.data(),RefreshMode::Half,false);
  assert(d.beginGrayscale16());assert(!d.displayCommitted());assert(d.commitGrayscale16(bw.data()));
  const auto beforeNativeCleanup=draws;d.cleanupGrayscaleBuffers(bus,bw.data());assert(draws==beforeNativeCleanup);
  d.begin(bus);assert(!d.displayCommitted());
  d.deepSleep(bus);assert(!d.displayCommitted());
  failAllocation=1;d.begin(bus);assert(!ready);d.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);assert(!d.displayCommitted());
  std::puts("ReadPico overlay-only, deferred/committed/failed/cancelled/BW fallback/native and allocation failure passed");
}
''')
    command=[os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Wno-unused-parameter','-DFREEINK_DRIVER_EPDIY_LCD=1','-DFREEINK_EPDIY_LCD_CONFIG=testConfig','-I'+str(root),'-I'+str(root/'include'),str(root/'test.cpp'),str(root/'src/driver/EpdiyLcdDriver.cpp'),'-o',str(root/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(root/'test')],check=True)
