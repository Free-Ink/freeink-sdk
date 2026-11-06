#!/usr/bin/env python3
"""Exercise the production sleep cleanup with recording panel stubs."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[2]/"src/driver/EpdiyDriver.cpp").read_text()
a=s.index("enum class Draw")
b=s.index("}  // namespace",a)
c=s.index("void EpdiyDriver::deepSleep(")
d=s.index("void EpdiyDriver::display(",c)
code=r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
using EpdWaveform=int;
static int g_hl,MY_WAVEFORM,fastWave;
static bool g_started=true,g_glassMatchesFront=true,g_resyncPending,powerGood=true;
static std::string calls;
static const EpdWaveform* selected;
enum EpdDrawError {EPD_DRAW_SUCCESS=0, EPD_DRAW_EMPTY_LINE_QUEUE=1, FAILED=2};
static EpdDrawError clearError=EPD_DRAW_SUCCESS,drawError=EPD_DRAW_SUCCESS;
#define LOG_INF(tag, ...) ((void)std::printf(__VA_ARGS__))
#define LOG_ERR(...) ((void)0)
static constexpr int MODE_GC16=0;
static int64_t esp_timer_get_time(){return 0;}
static void epd_poweron(){calls+='+';}
static void epd_poweroff(){calls+='-';}
namespace metalioplus {bool panelPowerGood(){return powerGood;}}
static int epd_ambient_temperature(){return 25;}
static const EpdWaveform* my_waveform_fast(){return &fastWave;}
static void epd_hl_waveform(int*,const EpdWaveform* wave){selected=wave;}
static EpdDrawError epd_hl_update_screen_full(int*,int,int){calls+='D';return drawError;}
static EpdDrawError epd_hl_update_screen(int*,int,int){calls+='D';return drawError;}
static EpdDrawError epd_hl_update_screen_from_white(int*,int,int){assert(selected==&MY_WAVEFORM);calls+='R';return drawError;}
static EpdDrawError epd_clear_checked(){calls+='C';return clearError;}
static int epd_last_leading_skip(){return 0;}
struct EpdBus {};
struct EpdiyDriver {void deepSleep(EpdBus&);};
"""+s[a:b]+s[c:d]+r"""
int main(){
 EpdBus bus; EpdiyDriver driver;
 driver.deepSleep(bus);assert(calls=="+CR--" && g_glassMatchesFront);
 calls.clear();g_started=false;driver.deepSleep(bus);assert(calls=="-");
 calls.clear();g_started=true;g_glassMatchesFront=false;driver.deepSleep(bus);assert(calls=="-");
 calls.clear();g_glassMatchesFront=true;powerGood=false;driver.deepSleep(bus);
 assert(calls=="+--" && !g_glassMatchesFront);
 calls.clear();powerGood=true;g_glassMatchesFront=true;clearError=FAILED;driver.deepSleep(bus);
 assert(calls=="+C--" && !g_glassMatchesFront);
 calls.clear();g_glassMatchesFront=true;clearError=EPD_DRAW_SUCCESS;drawError=FAILED;driver.deepSleep(bus);
 assert(calls=="+CR--" && !g_glassMatchesFront);
 puts("Sleep uses checked clear then full GC16 from white; invalid frames and failures handled");
}
"""
with tempfile.TemporaryDirectory(prefix="epdiy-sleep-") as tmp:
 p=Path(tmp);(p/"test.cpp").write_text(code)
 subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",str(p/"test.cpp"),"-o",str(p/"test")],check=True)
 subprocess.run([str(p/"test")],check=True)
