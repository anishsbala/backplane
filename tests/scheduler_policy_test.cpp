#include "scheduler_policy.h"

#include <cassert>
#include <iostream>

int main() {
  assert(scheduler_policy::shouldPreempt(2, 9));
  assert(!scheduler_policy::shouldPreempt(9, 2));
  assert(!scheduler_policy::shouldPreempt(5, 5));

  assert(scheduler_policy::recoveryPercent(1, 1000000, 950001) == 95);
  assert(scheduler_policy::recoveryPercent(1, 1000000, 1) == 0);
  assert(scheduler_policy::recoveryPercent(1, 1000000, 1000001) == 100);
  assert(scheduler_policy::recoveryPercent(10, 5, 10) == 100);

  std::cout << "scheduler policy tests passed\n";
  return 0;
}
