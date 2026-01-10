#pragma once

#include "task_record.h"

#include <hiredis/hiredis.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class RedisStore {
public:
  RedisStore(const std::string& host, int port, int lease_seconds);
  ~RedisStore();

  bool connect(std::string* error);

  bool submitTask(TaskRecord task, std::string* task_id, std::string* error);
  bool leaseTask(const std::string& worker_id, TaskRecord* task);
  bool heartbeat(const std::string& task_id, const std::string& worker_id);
  bool checkpoint(const std::string& task_id,
                  const std::string& worker_id,
                  int64_t checkpoint,
                  int64_t partial_result,
                  int progress_percent);
  bool completeTask(const std::string& task_id,
                    const std::string& worker_id,
                    bool success,
                    int64_t result,
                    const std::string& error);
  bool getTask(const std::string& task_id, TaskRecord* task);
  std::vector<TaskRecord> listTasks();
  std::vector<WorkerRecord> listWorkers();
  std::vector<EventRecord> listEvents(int limit);
  int requeueStaleTasks();
  bool clearAll(std::string* error);

private:
  bool readTaskLocked(const std::string& task_id, TaskRecord* task);
  bool isOwnedByLocked(const std::string& task_id, const std::string& worker_id);
  void writeTaskLocked(const TaskRecord& task);
  void enqueueLocked(const std::string& task_id, int priority);
  void touchWorkerLocked(const std::string& worker_id, const std::string& task_id);
  void clearWorkerTaskLocked(const std::string& worker_id);
  void appendEventLocked(const std::string& type,
                         const std::string& task_id,
                         const std::string& worker_id,
                         const std::string& message);
  int64_t nowSeconds() const;

  std::string host_;
  int port_;
  int lease_seconds_;
  redisContext* context_ = nullptr;
  std::mutex mutex_;
};
