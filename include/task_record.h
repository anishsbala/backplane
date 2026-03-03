#pragma once

#include <cstdint>
#include <string>

struct TaskRecord {
  std::string id;
  std::string name;
  int kind = 1;
  int64_t range_start = 0;
  int64_t range_end = 0;
  int chunk_size = 10000;
  int priority = 1;
  int max_attempts = 3;
  int attempts = 0;
  int preemptions = 0;
  std::string status = "QUEUED";
  std::string worker_id;
  int64_t checkpoint = 0;
  int64_t result = 0;
  int progress_percent = 0;
  std::string error;
  int64_t created_at = 0;
  int64_t updated_at = 0;
  int64_t lease_expires_at = 0;
};

struct WorkerRecord {
  std::string worker_id;
  std::string current_task_id;
  int64_t last_seen_at = 0;
};

struct EventRecord {
  int64_t id = 0;
  int64_t timestamp = 0;
  std::string type;
  std::string task_id;
  std::string worker_id;
  std::string message;
};
