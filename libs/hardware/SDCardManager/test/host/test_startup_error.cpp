#include <SDCardManager.h>

#include <cassert>
#include <cstdio>

int main() {
  SDCardManager manager;
  using Error = SDCardManager::StartupError;

  fakeSd::cardBeginFails = true;
  fakeSd::cardErrorCode = SD_CARD_ERROR_CMD0;
  assert(!manager.begin());
  assert(manager.startupError() == Error::CardNotDetected);

  fakeSd::cardErrorCode = 2;
  assert(!manager.begin());
  assert(manager.startupError() == Error::CardCommunicationFailure);

  fakeSd::cardErrorCode = 0;
  assert(!manager.begin());
  assert(manager.startupError() == Error::Unknown);

  fakeSd::cardBeginFails = false;
  fakeSd::volumeBeginFails = true;
  fakeSd::cardErrorCode = 0;
  assert(!manager.begin());
  assert(manager.startupError() == Error::VolumeMountFailed);

  fakeSd::cardErrorCode = 1;
  assert(!manager.begin());
  assert(manager.startupError() == Error::CardCommunicationFailure);

  fakeSd::volumeBeginFails = false;
  assert(manager.begin());
  assert(manager.startupError() == Error::None);

  std::puts("SD startup error classification tests passed");
}
