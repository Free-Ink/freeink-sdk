#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <Wire.h>
#include <BoardConfig.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#define private public
#include <InputManager.h>
#undef private

static void capture(InputManager& input, bool down, unsigned x=120, unsigned y=200, unsigned ms=10) {
  Wire.touch = {uint8_t(down), uint8_t(x >> 8), uint8_t(x), uint8_t(y >> 8), uint8_t(y)};
  fakeNow += ms;
  input.captureTouchSample();
}
static void tick(InputManager& input) { fakeNow += 10; input.update(); }
static void tap(InputManager& input) {
  capture(input, true);
  capture(input, true);
  capture(input, false);
}
static void idle(InputManager& input) {
  capture(input, false);
  for (int i=0; i<5; ++i) tick(input);
}

int main() {
  std::fill(std::begin(gpioLevels), std::end(gpioLevels), HIGH);
  InputManager input;
  input.begin();
  for (const uint32_t caps : {MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM}) {
    failAllocationCaps=caps;
    assert(!input.beginBufferedTouch() && !input.touchQueue);
    assert(!input.touchQueueControl && !input.touchQueueStorage && fakeHeapAllocations.empty());
  }
  failAllocationCaps=0;
  failQueueCreate=true;
  assert(!input.beginBufferedTouch() && !input.touchQueue);
  assert(fakeHeapAllocations.empty());
  failQueueCreate=false;
  // The task stub refuses creation; the allocated queue must be cleaned up.
  assert(!input.beginBufferedTouch() && !input.touchQueue && !input.touchWorkerRunning);
  assert(fakeHeapAllocations.empty());
  // Drive the real producer deterministically, without running a desktop scheduler.
  input.touchQueue=xQueueCreate(InputManager::TOUCH_QUEUE_LENGTH, sizeof(InputManager::BufferedTouchSample));
  // Idle/error-only reports must not wake the main loop every sampler tick.
  capture(input,false);
  assert(!input.touchContactPending());
  tick(input);
  Wire.failTouch=true;
  capture(input,false);
  assert(!input.touchContactPending());
  Wire.failTouch=false;
  tick(input);
  float x,y,x2,y2;
  unsigned long held;

  tap(input);
  assert(input.touchContactPending());
  fakeNow += 1200; // UI was blocked; acquisition timestamps must survive.
  tick(input);
  assert(input.wasTouchPressedAt(x,y));
  assert(input.isTouchTapCandidate(x,y,held) && held <= 10);
  assert(!input.wasTouchLongPress(x,y));
  tick(input);
  assert(input.wasTouchTap(x,y) && input.lastTouchHeldMs()==20);
  tick(input);
  assert(!input.wasTouchTap(x,y));

  tap(input); tap(input);
  unsigned taps=0;
  for(int i=0;i<6;++i) { tick(input); taps += input.wasTouchTap(x,y); }
  assert(taps==2);

  capture(input,true); capture(input,true);
  capture(input,true,120,400,50); capture(input,false,120,400);
  fakeNow+=1200;
  tick(input); tick(input);
  assert(input.wasSwipe(x,y,x2,y2) && !input.wasTouchTap(x,y));
  assert(input.lastTouchHeldMs()==70);

  capture(input,true); capture(input,true);
  for(int i=0;i<55;++i) capture(input,true);
  capture(input,false);
  tick(input); tick(input);
  assert(input.wasTouchLongPress(x,y));
  input.suppressTouchContact();
  tick(input);
  assert(!input.wasTouchTap(x,y));
  idle(input);

  // In-app suppression drops only that gesture; a following new tap still works.
  tap(input);
  tick(input);
  input.suppressTouchContact();
  tick(input);
  assert(!input.wasTouchTap(x,y));
  tap(input);
  tick(input); tick(input);
  assert(input.wasTouchTap(x,y));

  // A screen change also rejects a read that began before the discard.
  capture(input,true); capture(input,true);
  tick(input);
  const auto oldGeneration=input.touchGeneration.load();
  input.discardBufferedTouch();
  InputManager::BufferedTouchSample stale{};
  stale.ok=true; stale.generation=oldGeneration; stale.atMs=fakeNow;
  xQueueSend(input.touchQueue,&stale,0); // old idle sample must not unlock the guard
  capture(input,true); capture(input,true); capture(input,false);
  for(int i=0;i<4;++i) { tick(input); assert(!input.wasTouchTap(x,y) && !input.wasTouchPressed()); }
  tap(input); tick(input); tick(input);
  assert(input.wasTouchTap(x,y));

  input.discardBufferedTouch();
  Wire.failTouch = true;
  capture(input,false,0,0,110);
  tick(input);
  Wire.failTouch = false;
  tap(input); tick(input); tick(input);
  assert(input.wasTouchTap(x,y)); // idle NACKs must not leave the transition guard stuck

  // Overflow cancels the whole contact rather than inventing a release/tap.
  capture(input,true); capture(input,true); tick(input);
  for(unsigned i=0;i<InputManager::TOUCH_QUEUE_LENGTH+2;++i) capture(input,true);
  tick(input);
  assert(!input.isTouchPressed() && !input.wasTouchTap(x,y));
  capture(input,true); capture(input,false);
  tick(input);
  assert(!input.wasTouchTap(x,y));
  tap(input); tick(input); tick(input);
  assert(input.wasTouchTap(x,y));

  // Read errors release a previously accepted contact after the existing timeout.
  capture(input,true); capture(input,true); tick(input);
  Wire.failTouch=true;
  capture(input,true,120,200,110);
  Wire.failTouch=false;
  tick(input);
  assert(input.wasTouchReleased() && !input.isTouchPressed());
  idle(input);

  const unsigned homeY=BoardConfig::isMetalioEInk4Plus() ? 1600 : 900;
  capture(input,true,100,homeY); capture(input,false);
  tick(input); assert(input.wasHomeKeyPressed());
  tick(input); assert(input.wasHomeKeyTapped());
  assert(input.touchFrameTime() == input.bufferedTouchAt);
  capture(input,true,100,homeY);
  capture(input,true,100,homeY,710); capture(input,false);
  tick(input); tick(input); assert(input.wasHomeKeyLongPressed());
  tick(input); assert(!input.wasHomeKeyTapped());

  // Buffered bezel-key pulses survive the physical-button debounce.
  capture(input,true,240,homeY); capture(input,false);
  unsigned presses=0, releases=0;
  const auto key=BoardConfig::isMetalioEInk4Plus() ? InputManager::BTN_UP : InputManager::BTN_DOWN;
  for(int i=0;i<6;++i) { tick(input); presses+=input.wasPressed(key); releases+=input.wasReleased(key); }
  assert(presses==1 && releases==1 && !input.isPressed(key));
  capture(input,true,240,homeY); capture(input,false);
  input.update();
  fakeNow+=5; input.update();
  assert(!input.wasPressed(key));
  fakeNow+=1; input.update();
  assert(input.wasPressed(key));
  input.update(); // start release debounce
  fakeNow+=5; input.update();
  assert(!input.wasReleased(key));
  fakeNow+=1; input.update();
  assert(input.wasReleased(key));

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // A buffered single-sample contact retains both edges and becomes a tap.
  capture(input,true); capture(input,false); tick(input);
  assert(input.wasTouchPressed() && input.isTouchPressed());
  tick(input);
  assert(input.wasTouchReleased() && input.wasTouchTap(x,y));
  const auto two = [&](unsigned ax, unsigned ay, unsigned bx, unsigned by) {
    Wire.touch={2,uint8_t(ax>>8),uint8_t(ax),uint8_t(ay>>8),uint8_t(ay),0,0,
                uint8_t(bx>>8),uint8_t(bx),uint8_t(0x10|(by>>8)),uint8_t(by)};
    fakeNow+=10; input.captureTouchSample();
  };
  for (int gesture=0; gesture<3; ++gesture) {
    idle(input);
    two(120,200,300,200); two(120,200,300,200);
    if (gesture==0) two(60,200,360,200);  // spread
    if (gesture==1) two(210,110,210,290); // rotate
    if (gesture==2) two(120,300,300,300); // two-finger swipe
    capture(input,false);
    fakeNow+=1200;
    tick(input); tick(input);
    float value;
    uint8_t count;
    assert(!input.wasTouchTap(x,y));
    if (gesture==0) assert(input.wasMultiTouchPinch(value,x,y,held) && value>1.6f);
    if (gesture==1) assert(input.wasMultiTouchRotation(value,x,y,held) && (value>80 || value< -80));
    if (gesture==2) assert(input.wasMultiTouchSwipe(count,x,y,x2,y2,held) && count==2);
    assert(held==30); // Gesture tracking now starts on the first sample.
  }
#endif
  // A queued bezel release must not turn a page after screen ownership changes.
  idle(input);
  capture(input,true,240,homeY);
  tick(input); tick(input);
  assert(input.isPressed(key));
  input.currentState |= 1u << InputManager::BTN_POWER;
  input.discardBufferedTouch();
  assert(!input.isPressed(key) && input.isPressed(InputManager::BTN_POWER));
  capture(input,false);
  tick(input);
  assert(!input.wasReleased(key));

  input.endBufferedTouch();
  assert(!input.touchQueue && !input.touchWorkerRunning);
  // Exercise real producer start/stop and cleanup with a desktop task; no concurrent fixture writes.
  Wire.touch = {};
  fakeTaskEnabled = true;
  assert(input.beginBufferedTouch() && input.beginBufferedTouch());
  assert(fakeHeapAllocations.size()==2);
  const auto storage=fakeHeapAllocations.at(input.touchQueueStorage);
  const auto control=fakeHeapAllocations.at(input.touchQueueControl);
  assert(storage.size==5120 && storage.caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  assert(control.size==sizeof(StaticQueue_t) && control.caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  assert(static_cast<FakeQueue*>(input.touchQueue)->storage==input.touchQueueStorage);
  for (unsigned i=0; i<100 && !uxQueueMessagesWaiting(input.touchQueue); ++i) vTaskDelay(1);
  assert(uxQueueMessagesWaiting(input.touchQueue));
  assert(input.prepareForDeepSleep());
  assert(!input.touchQueue && !input.touchWorkerRunning);
  assert(fakeHeapAllocations.empty());
  assert(input.beginBufferedTouch());
  input.endBufferedTouch();
  input.endBufferedTouch();
  assert(fakeHeapAllocations.empty());
  {
    InputManager temporary;
    temporary.begin();
    assert(temporary.beginBufferedTouch());
  }
  assert(fakeHeapAllocations.empty());
  fakeTaskEnabled = false;
  std::puts("Buffered touch: delayed taps, ordering, timestamps, gestures, overflow, discard, keys and allocation failures passed");
}
