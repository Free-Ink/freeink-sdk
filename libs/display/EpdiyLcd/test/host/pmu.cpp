#include <BoardReadPico.cpp>
#include <chrono>
#include <thread>

void delay(unsigned long) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

int main() {
  BoardReadPico::detail::beginI2C();
  BoardReadPico::g_pmuPresent = true;
  std::thread clock([] {
    for (int i = 0; i < 30; ++i) {
      uint32_t seconds;
      bool synced;
      assert(BoardReadPico::pmuTimeGet(seconds, synced));
      assert(seconds == 0x12345678 && synced);
    }
  });
  std::thread panel([] {
    for (int i = 0; i < 30; ++i) assert(BoardReadPico::pmuVcomMv() == 1500);
  });
  std::thread input([] {
    for (int i = 0; i < 30; ++i) {
      assert(BoardReadPico::pmuPoll());
      assert(BoardReadPico::powerKeyPressed());
    }
  });
  clock.join();
  panel.join();
  input.join();
  assert(Wire.commands == 61 && !Wire.inFlight);
#if FREEINK_READPICO_DIAGNOSTICS
  assert(BoardReadPico::g_pmuTiming.commands == 60);
  assert(BoardReadPico::g_pmuTiming.maxHoldUs >= 1000);
#endif
}
