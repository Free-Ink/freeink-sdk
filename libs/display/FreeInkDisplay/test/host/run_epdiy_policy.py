#!/usr/bin/env python3
"""Check production EPDiy refresh selection without panel hardware."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'src/driver/EpdiyDriver.cpp').read_text()
a=source.index('void EpdiyDriver::display(')
b=source.index('void EpdiyDriver::copyGrayscaleLsb(',a)
code=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
struct EpdBus {};
enum class RefreshMode {Full,Half,Fast};
enum class Draw {Gc16Reinforced,Gc16All,Clean};
static bool g_started=true,g_basePending,g_resyncPending,g_firstFrameAllPixels,g_glassMatchesFront=true;
static unsigned changed,calls;
static Draw last;
static uint32_t expandFrame(const uint8_t*) {return changed;}
static void drawFront(Draw how) {calls++;last=how;}
struct EpdiyDriver {void display(EpdBus&,const uint8_t*,const uint8_t*,RefreshMode,bool);};
'''+source[a:b]+r'''
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
 puts("EPDiy refresh policy: small/large changes, no-op, invalid baseline, cleanup and startup passed");
}
'''
code='#include <initializer_list>\n'+code
with tempfile.TemporaryDirectory(prefix='epdiy-policy-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
