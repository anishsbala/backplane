#pragma once

#include <algorithm>
#include <cstdint>

namespace scheduler_policy {

inline bool shouldPreempt(int running_priority, int queued_priority) {
  return queued_priority > running_priority;
}

inline int recoveryPercent(int64_t range_start,
                           int64_t range_end,
                           int64_t checkpoint) {
  if (range_end < range_start) return 100;
  const int64_t total = range_end - range_start + 1;
  const int64_t completed = std::clamp(checkpoint - range_start, int64_t{0}, total);
  return static_cast<int>((completed * 100) / total);
}

}  // namespace scheduler_policy
