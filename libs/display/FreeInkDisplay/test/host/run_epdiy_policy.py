#!/usr/bin/env python3
"""Check production EPDiy refresh selection without panel hardware."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'src/driver/EpdiyDriver.cpp').read_text()
a=source.index('void EpdiyDriver::display(')
b=source.index('void EpdiyDriver::copyGrayscaleLsb(',a)
tone=source[source.index('constexpr uint8_t kInkLevel'):source.index('// How a frame reaches the glass.')]
g=source.index('bool EpdiyDriver::displayGray16Image(')
gray=source[g:source.index('PanelDriver& epdiyDriver()',g)]
code=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
struct EpdBus {};
enum class RefreshMode {Full,Half,Fast};
enum class Draw {Gc16Reinforced,Gc16All,Clean,Tuned};
static bool g_started=true,g_basePending,g_resyncPending,g_firstFrameAllPixels,g_glassMatchesFront=true;
static unsigned changed,calls;
static Draw last;
static uint32_t expandFrame(const uint8_t*) {return changed;}
static bool drawOk=true;
static void drawFront(Draw how) {calls++;last=how;g_glassMatchesFront=drawOk;}
struct Geometry {uint32_t bufferSize;};
static uint8_t frontFb[8];
static int g_hl;
static uint8_t* epd_hl_get_framebuffer(void*) {return frontFb;}
struct EpdiyDriver {
 Geometry geometry() const {return {2};}
 void display(EpdBus&,const uint8_t*,const uint8_t*,RefreshMode,bool);
 bool displayGray16Image(EpdBus&,const uint8_t*,bool,bool);
};
'''+tone+source[a:b]+gray+r'''
int main() {
 EpdBus bus; EpdiyDriver driver; uint8_t fb=0;
 auto run=[&](RefreshMode mode) {calls=0;driver.display(bus,&fb,nullptr,mode,false);};
 for(unsigned n : {1u,100000u}) {changed=n;run(RefreshMode::Fast);assert(calls==1 && last==Draw::Gc16Reinforced);}
 changed=0;run(RefreshMode::Fast);assert(calls==0);
 g_glassMatchesFront=false;run(RefreshMode::Fast);assert(calls==1 && last==Draw::Gc16Reinforced);
 g_glassMatchesFront=true;
 run(RefreshMode::Half);assert(calls==1 && last==Draw::Gc16All);
 run(RefreshMode::Full);assert(calls==1 && last==Draw::Clean);
 g_firstFrameAllPixels=true;run(RefreshMode::Fast);assert(calls==1 && last==Draw::Gc16All && !g_firstFrameAllPixels);
 g_resyncPending=true;run(RefreshMode::Fast);assert(calls==1 && last==Draw::Gc16All && !g_resyncPending);
 g_basePending=true;changed=1;run(RefreshMode::Fast);assert(!g_basePending);
 g_started=false;run(RefreshMode::Fast);assert(calls==0);
 uint8_t image[8]={0xB1,0xF0};  // ink 11,1 then 15,0; the rest white
 assert(!driver.displayGray16Image(bus,image,true,false) && calls==0);
 g_started=true;g_glassMatchesFront=true;
 // 16-level image: mapped into the front buffer, left pixel to the low nibble.
 g_basePending=true;calls=0;
 assert(driver.displayGray16Image(bus,image,true,false) && calls==1 && last==Draw::Gc16All && !g_basePending);
 assert(frontFb[0]==0xD4 && frontFb[1]==0xF0 && frontFb[2]==0xFF);
 assert(!driver.displayGray16Image(bus,nullptr,true,false) && calls==1);
 // fast: changed pixels only, and nothing at all for the image already up.
 calls=0;assert(driver.displayGray16Image(bus,image,true,true) && calls==0);
 image[3]=0x08;assert(driver.displayGray16Image(bus,image,true,true) && calls==1 && last==Draw::Gc16Reinforced);
 assert(frontFb[3]==0x6F);
 g_glassMatchesFront=false;calls=0;assert(driver.displayGray16Image(bus,image,true,true) && calls==1);
 g_resyncPending=true;assert(driver.displayGray16Image(bus,image,true,true) && last==Draw::Gc16All && !g_resyncPending);
 drawOk=false;assert(!driver.displayGray16Image(bus,image,true,false));drawOk=true;
 // Calibration table in use: ink i is level 15 - i and fast is the tuned draw;
 // a clean frame is still the built-in GC16 on every pixel.
 g_tuned=true;uint8_t direct[8]={0xB1,0xF0};
 calls=0;assert(driver.displayGray16Image(bus,direct,true,true) && calls==1 && last==Draw::Tuned);
 assert(frontFb[0]==0xE4 && frontFb[1]==0xF0 && frontFb[2]==0xFF);
 assert(driver.displayGray16Image(bus,direct,true,false) && last==Draw::Gc16All);
 g_resyncPending=true;assert(driver.displayGray16Image(bus,direct,true,true) && last==Draw::Gc16All && !g_resyncPending);
 g_tuned=false;
 puts("EPDiy refresh policy: small/large changes, no-op, invalid baseline, cleanup, startup and 16-level image passed");
}
'''
code='#include <initializer_list>\n'+code
with tempfile.TemporaryDirectory(prefix='epdiy-policy-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
