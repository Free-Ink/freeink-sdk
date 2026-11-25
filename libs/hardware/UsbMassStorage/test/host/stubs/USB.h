#pragma once
struct FakeUsb {
  bool begin() { return beginOk; }
  bool beginOk = true;
};
inline FakeUsb USB;
