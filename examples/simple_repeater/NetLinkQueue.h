#pragma once

#include <MeshCore.h>
#include <cstdint>
#include <memory>
#include <new>

namespace netlink {

struct Frame {
  uint32_t number, born, due, firstSent;
  uint8_t size, attempts, bytes[MAX_TRANS_UNIT];
};

class FrameQueue {
  std::unique_ptr<Frame[]> frames;
  unsigned capacity_ = 0;
  unsigned count_ = 0;

public:
  unsigned capacity() const { return capacity_; }
  unsigned count() const { return count_; }
  Frame *begin() { return frames.get(); }
  Frame *end() { return capacity_ ? frames.get() + capacity_ : nullptr; }

  bool allocate(unsigned size) {
    std::unique_ptr<Frame[]> next(size ? new (std::nothrow) Frame[size]() : nullptr);
    if (size && !next) return false;
    frames = std::move(next);
    capacity_ = size;
    count_ = 0;
    return true;
  }

  Frame &insert(uint32_t number) {
    Frame *dest = begin();
    for (auto &f : *this) {
      if (!f.size) {
        dest = &f;
        ++count_;
        break;
      }
      if (int32_t(f.number - dest->number) < 0) dest = &f;
    }
    dest->number = number;
    return *dest;
  }

  // Changing the capacity is an administrative action, not a delivery failure.
  void retainNewest(FrameQueue &old) {
    if (!capacity_) return;
    for (auto &f : old) {
      if (!f.size) continue;
      if (count_ == capacity_) {
        bool newer = false;
        for (auto &kept : *this)
          if (int32_t(f.number - kept.number) > 0) newer = true;
        if (!newer) continue;
      }
      insert(f.number) = f;
    }
  }

  void remove(Frame &f) {
    f.size = 0;
    --count_;
  }
};

} // namespace netlink
