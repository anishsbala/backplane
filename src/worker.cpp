#include "task_print.h"

#include "backplane.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

bool isPrime(int64_t value) {
  if (value < 2) return false;
  if (value == 2) return true;
  if (value % 2 == 0) return false;

  for (int64_t i = 3; i <= value / i; i += 2) {
    if (value % i == 0) return false;
  }

  return true;
}

int progressFor(int64_t next_value, int64_t start, int64_t end) {
  if (end < start) return 100;

  int64_t total = end - start + 1;
  int64_t finished = std::max<int64_t>(0, next_value - start);
  int progress = static_cast<int>((finished * 100) / total);
  return std::max(0, std::min(100, progress));
}

class Worker {
public:
  Worker(const std::string& address,
         const std::string& worker_id,
         int crash_after,
         int fail_after,
         int sleep_ms,
         bool once)
      : worker_id_(worker_id),
        crash_after_(crash_after),
        fail_after_(fail_after),
        sleep_ms_(sleep_ms),
        once_(once) {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    stub_ = backplane::BackplaneService::NewStub(channel);
  }

  void run() {
    std::cout << worker_id_ << " waiting for tasks\n";

    while (true) {
      backplane::Task task;
      if (!leaseTask(&task)) {
        if (once_) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      printTask(task);
      runPrimeCount(task);

      if (once_) return;
    }
  }

private:
  bool leaseTask(backplane::Task* task) {
    grpc::ClientContext context;
    backplane::LeaseTaskRequest request;
    backplane::LeaseTaskReply reply;

    request.set_worker_id(worker_id_);
    grpc::Status status = stub_->LeaseTask(&context, request, &reply);

    if (!status.ok() || !reply.found()) {
      return false;
    }

    *task = reply.task();
    return true;
  }

  bool heartbeatTask(const backplane::Task& task) {
    grpc::ClientContext context;
    backplane::HeartbeatRequest request;
    backplane::BasicReply reply;

    request.set_task_id(task.id());
    request.set_worker_id(worker_id_);

    grpc::Status status = stub_->Heartbeat(&context, request, &reply);
    return status.ok() && reply.ok();
  }

  bool saveCheckpoint(const backplane::Task& task,
                      int64_t checkpoint,
                      int64_t partial_result,
                      int progress) {
    grpc::ClientContext context;
    backplane::CheckpointRequest request;
    backplane::BasicReply reply;

    request.set_task_id(task.id());
    request.set_worker_id(worker_id_);
    request.set_checkpoint(checkpoint);
    request.set_partial_result(partial_result);
    request.set_progress_percent(progress);

    grpc::Status status = stub_->Checkpoint(&context, request, &reply);
    return status.ok() && reply.ok();
  }

  bool completeTask(const backplane::Task& task, int64_t result) {
    grpc::ClientContext context;
    backplane::CompleteTaskRequest request;
    backplane::BasicReply reply;

    request.set_task_id(task.id());
    request.set_worker_id(worker_id_);
    request.set_success(true);
    request.set_result(result);

    grpc::Status status = stub_->CompleteTask(&context, request, &reply);
    return status.ok() && reply.ok();
  }

  bool failTask(const backplane::Task& task, const std::string& error, int64_t result) {
    grpc::ClientContext context;
    backplane::CompleteTaskRequest request;
    backplane::BasicReply reply;

    request.set_task_id(task.id());
    request.set_worker_id(worker_id_);
    request.set_success(false);
    request.set_result(result);
    request.set_error(error);

    grpc::Status status = stub_->CompleteTask(&context, request, &reply);
    return status.ok() && reply.ok();
  }

  bool maybePreempt(const backplane::Task& task) {
    grpc::ClientContext context;
    backplane::MaybePreemptRequest request;
    backplane::MaybePreemptReply reply;

    request.set_task_id(task.id());
    request.set_worker_id(worker_id_);
    grpc::Status status = stub_->MaybePreempt(&context, request, &reply);
    if (status.ok() && reply.preempted()) {
      std::cout << worker_id_ << " yielded " << task.id()
                << " for queued priority " << reply.queued_priority() << "\n";
      return true;
    }
    return false;
  }

  void runPrimeCount(const backplane::Task& task) {
    if (task.kind() != backplane::PRIME_COUNT) {
      failTask(task, "worker only supports PRIME_COUNT", task.result());
      return;
    }

    int64_t cursor = std::max(task.checkpoint(), task.range_start());
    int64_t result = task.result();
    int chunk_size = task.chunk_size() <= 0 ? 10000 : task.chunk_size();
    int checkpoints = 0;

    while (cursor <= task.range_end()) {
      if (!heartbeatTask(task)) {
        std::cout << worker_id_ << " lost ownership of " << task.id() << " before work started\n";
        return;
      }

      int64_t chunk_end = std::min<int64_t>(task.range_end(), cursor + chunk_size - 1);

      for (int64_t value = cursor; value <= chunk_end; value++) {
        if (isPrime(value)) result += 1;
      }

      cursor = chunk_end + 1;
      checkpoints += 1;
      int progress = progressFor(cursor, task.range_start(), task.range_end());

      if (!saveCheckpoint(task, cursor, result, progress)) {
        std::cout << worker_id_ << " lost ownership of " << task.id() << "\n";
        return;
      }

      std::cout << worker_id_ << " checkpointed " << task.id()
                << " at " << cursor
                << " (" << progress << "%, result=" << result << ")\n";

      if (fail_after_ > 0 && checkpoints >= fail_after_) {
        std::cout << worker_id_ << " simulating a normal task failure\n";
        failTask(task, "simulated worker error", result);
        return;
      }

      if (crash_after_ > 0 && checkpoints >= crash_after_) {
        std::cout << worker_id_ << " simulating crash after checkpoint " << checkpoints << "\n";
        std::exit(2);
      }

      if (maybePreempt(task)) {
        return;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms_));
    }

    if (completeTask(task, result)) {
      std::cout << worker_id_ << " completed " << task.id() << " with result " << result << "\n";
    } else {
      std::cout << worker_id_ << " could not complete " << task.id() << "\n";
    }
  }

  std::string worker_id_;
  int crash_after_ = 0;
  int fail_after_ = 0;
  int sleep_ms_ = 100;
  bool once_ = false;
  std::unique_ptr<backplane::BackplaneService::Stub> stub_;
};

void usage() {
  std::cout << "Usage:\n";
  std::cout << "  backplane_worker <coordinator_addr> <worker_id> [--crash-after N] [--fail-after N] [--sleep-ms N] [--once]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 1;
  }

  std::string address = argv[1];
  std::string worker_id = argv[2];
  int crash_after = 0;
  int fail_after = 0;
  int sleep_ms = 100;
  bool once = false;

  for (int i = 3; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--crash-after" && i + 1 < argc) {
      crash_after = std::stoi(argv[++i]);
    } else if (arg == "--fail-after" && i + 1 < argc) {
      fail_after = std::stoi(argv[++i]);
    } else if (arg == "--sleep-ms" && i + 1 < argc) {
      sleep_ms = std::stoi(argv[++i]);
    } else if (arg == "--once") {
      once = true;
    } else {
      usage();
      return 1;
    }
  }

  Worker worker(address, worker_id, crash_after, fail_after, sleep_ms, once);
  worker.run();
  return 0;
}
