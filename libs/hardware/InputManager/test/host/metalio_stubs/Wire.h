#pragma once
#include <cstdint>
#include <vector>
#include <array>
struct WireStub {
  std::array<uint8_t,8> expander{255,255,255,255,0,0,255,255};
  std::array<uint8_t,5> touch{};
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint8_t> tx,rx;
  uint8_t address=0,reg=0;
  unsigned offset=0;
  bool failExpander=false, failTouch=false;
  bool begin(int,int,uint32_t) { return true; }
  void setTimeOut(int) {}
  void beginTransmission(uint8_t a) { address=a; tx.clear(); }
  void write(uint8_t v) { tx.push_back(v); }
  void write(const uint8_t* v,size_t n) { tx.insert(tx.end(),v,v+n); }
  int endTransmission(bool=true) {
    if ((address==0x20 && failExpander) || (address==0x15 && failTouch)) return 1;
    if (!tx.empty()) reg=tx[0];
    if (address==0x20 && tx.size()>1) {
      writes.push_back(tx);
      for(size_t i=1;i<tx.size();++i) expander.at(reg+i-1)=tx[i];
    }
    return 0;
  }
  int requestFrom(uint8_t a,uint8_t n,uint8_t) {
    rx.clear(); offset=0;
    if(a==0x20 && !failExpander) for(unsigned i=0;i<n;++i) rx.push_back(expander.at(reg+i));
    if(a==0x15 && !failTouch && reg==2) rx.assign(touch.begin(),touch.end());
    return rx.size();
  }
  int available() { return rx.size()-offset; }
  uint8_t read() { return rx.at(offset++); }
};
inline WireStub Wire;
