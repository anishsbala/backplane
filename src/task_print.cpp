#include "task_print.h"

#include <iostream>

backplane::TaskStatus statusFromString(const std::string& status) {
  if (status == "QUEUED") return backplane::QUEUED;
  if (status == "RUNNING") return backplane::RUNNING;
  if (status == "DONE") return backplane::DONE;
  if (status == "FAILED") return backplane::FAILED;
  return backplane::TASK_STATUS_UNKNOWN;
}

std::string statusToString(backplane::TaskStatus status) {
  switch (status) {
    case backplane::QUEUED:
      return "QUEUED";
    case backplane::RUNNING:
      return "RUNNING";
    case backplane::DONE:
      return "DONE";
    case backplane::FAILED:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

void fillProtoTask(const TaskRecord& record, backplane::Task* task) {
  task->set_id(record.id);
  task->set_name(record.name);
  task->set_kind(static_cast<backplane::TaskKind>(record.kind));
  task->set_range_start(record.range_start);
  task->set_range_end(record.range_end);
  task->set_chunk_size(record.chunk_size);
  task->set_priority(record.priority);
  task->set_max_attempts(record.max_attempts);
  task->set_attempts(record.attempts);
  task->set_preemptions(record.preemptions);
  task->set_status(statusFromString(record.status));
  task->set_worker_id(record.worker_id);
  task->set_checkpoint(record.checkpoint);
  task->set_result(record.result);
  task->set_progress_percent(record.progress_percent);
  task->set_error(record.error);
  task->set_created_at(record.created_at);
  task->set_updated_at(record.updated_at);
  task->set_lease_expires_at(record.lease_expires_at);
}

void fillProtoWorker(const WorkerRecord& record, backplane::WorkerInfo* worker) {
  worker->set_worker_id(record.worker_id);
  worker->set_current_task_id(record.current_task_id);
  worker->set_last_seen_at(record.last_seen_at);
}

void fillProtoEvent(const EventRecord& record, backplane::Event* event) {
  event->set_id(record.id);
  event->set_timestamp(record.timestamp);
  event->set_type(record.type);
  event->set_task_id(record.task_id);
  event->set_worker_id(record.worker_id);
  event->set_message(record.message);
}

void printTask(const backplane::Task& task) {
  std::cout << "Task " << task.id() << "\n";
  std::cout << "  name:        " << task.name() << "\n";
  std::cout << "  status:      " << statusToString(task.status()) << "\n";
  std::cout << "  priority:    " << task.priority() << "\n";
  std::cout << "  attempts:    " << task.attempts() << "/" << task.max_attempts() << "\n";
  std::cout << "  preemptions: " << task.preemptions() << "\n";
  std::cout << "  worker:      " << (task.worker_id().empty() ? "-" : task.worker_id()) << "\n";
  std::cout << "  range:       " << task.range_start() << " to " << task.range_end() << "\n";
  std::cout << "  checkpoint:  " << task.checkpoint() << "\n";
  std::cout << "  progress:    " << task.progress_percent() << "%\n";
  std::cout << "  result:      " << task.result() << "\n";

  if (task.lease_expires_at() > 0) {
    std::cout << "  lease until: " << task.lease_expires_at() << "\n";
  }

  if (!task.error().empty()) {
    std::cout << "  error:       " << task.error() << "\n";
  }
}
