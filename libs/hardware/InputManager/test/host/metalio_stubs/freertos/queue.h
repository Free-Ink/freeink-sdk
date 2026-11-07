#pragma once
#include "FreeRTOS.h"
#include <cstring>
#include <mutex>
#include <new>
struct FakeQueue {
  unsigned capacity, itemSize;
  std::mutex mutex;
  unsigned char* storage;
  bool staticControl;
  unsigned count=0, head=0, tail=0;
};
using StaticQueue_t=FakeQueue;
inline bool failQueueCreate = false;
inline void* xQueueCreate(int n,int size) {
  if(failQueueCreate) return nullptr;
  return new FakeQueue{unsigned(n),unsigned(size),{},new unsigned char[n*size],false};
}
inline void* xQueueCreateStatic(unsigned n,unsigned size,unsigned char* storage,StaticQueue_t* control) {
  if(failQueueCreate) return nullptr;
  return new (control) FakeQueue{n,size,{},storage,true};
}
inline int xQueueSend(void* q,const void* data,int) {
  auto& queue=*static_cast<FakeQueue*>(q);
  std::lock_guard<std::mutex> lock(queue.mutex);
  if(queue.count==queue.capacity) return 0;
  std::memcpy(queue.storage+queue.head*queue.itemSize,data,queue.itemSize);
  queue.head=(queue.head+1)%queue.capacity;
  ++queue.count;
  return pdTRUE;
}
inline int xQueueReceive(void* q,void* out,int) {
  auto& queue=*static_cast<FakeQueue*>(q);
  std::lock_guard<std::mutex> lock(queue.mutex);
  if(!queue.count) return 0;
  std::memcpy(out,queue.storage+queue.tail*queue.itemSize,queue.itemSize);
  queue.tail=(queue.tail+1)%queue.capacity;
  --queue.count;
  return pdTRUE;
}
inline void xQueueReset(void* q) {
  auto& queue=*static_cast<FakeQueue*>(q);
  std::lock_guard<std::mutex> lock(queue.mutex);
  queue.count=queue.head=queue.tail=0;
}
inline unsigned uxQueueMessagesWaiting(void* q) {
  auto& queue=*static_cast<FakeQueue*>(q);
  std::lock_guard<std::mutex> lock(queue.mutex);
  return queue.count;
}
inline void vQueueDelete(void* q) {
  auto* queue=static_cast<FakeQueue*>(q);
  if(queue->staticControl) queue->~FakeQueue();
  else { delete[] queue->storage; delete queue; }
}
