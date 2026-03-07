#include "redis_store.h"
#include "scheduler_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {

const char FIELD_SEP = '\x1f';

struct ReplyDeleter {
  void operator()(redisReply* reply) const {
    if (reply != nullptr) freeReplyObject(reply);
  }
};

using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

int64_t toInt64(const std::string& value, int64_t fallback = 0) {
  if (value.empty()) return fallback;

  try {
    return std::stoll(value);
  } catch (...) {
    return fallback;
  }
}

int toInt(const std::string& value, int fallback = 0) {
  return static_cast<int>(toInt64(value, fallback));
}

bool replyOk(redisReply* reply) {
  if (reply == nullptr) return false;
  if (reply->type == REDIS_REPLY_STATUS) return true;
  if (reply->type == REDIS_REPLY_INTEGER) return true;
  return false;
}

std::string getField(const std::map<std::string, std::string>& fields,
                     const std::string& name,
                     const std::string& fallback = "") {
  auto it = fields.find(name);
  if (it == fields.end()) return fallback;
  return it->second;
}

std::map<std::string, std::string> readHash(redisReply* reply) {
  std::map<std::string, std::string> fields;
  if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) return fields;

  for (size_t i = 0; i + 1 < reply->elements; i += 2) {
    std::string key = reply->element[i]->str == nullptr ? "" : reply->element[i]->str;
    std::string value = reply->element[i + 1]->str == nullptr ? "" : reply->element[i + 1]->str;
    fields[key] = value;
  }

  return fields;
}

std::vector<std::string> splitEvent(const std::string& text) {
  std::vector<std::string> parts;
  std::string current;

  for (char ch : text) {
    if (ch == FIELD_SEP) {
      parts.push_back(current);
      current.clear();
    } else {
      current += ch;
    }
  }

  parts.push_back(current);
  return parts;
}

}  // namespace

RedisStore::RedisStore(const std::string& host, int port, int lease_seconds)
    : host_(host), port_(port), lease_seconds_(lease_seconds) {}

RedisStore::~RedisStore() {
  if (context_ != nullptr) {
    redisFree(context_);
    context_ = nullptr;
  }
}

bool RedisStore::connect(std::string* error) {
  std::lock_guard<std::mutex> guard(mutex_);
  context_ = redisConnect(host_.c_str(), port_);

  if (context_ == nullptr) {
    if (error != nullptr) *error = "Redis connection failed";
    return false;
  }

  if (context_->err) {
    if (error != nullptr) *error = context_->errstr;
    return false;
  }

  ReplyPtr pong(static_cast<redisReply*>(redisCommand(context_, "PING")));
  if (pong == nullptr || pong->type == REDIS_REPLY_ERROR) {
    if (error != nullptr) *error = "Redis did not reply to PING";
    return false;
  }

  return true;
}

bool RedisStore::submitTask(TaskRecord task, std::string* task_id, std::string* error) {
  std::lock_guard<std::mutex> guard(mutex_);

  ReplyPtr id_reply(static_cast<redisReply*>(redisCommand(context_, "INCR backplane:next_task_id")));
  if (id_reply == nullptr || id_reply->type != REDIS_REPLY_INTEGER) {
    if (error != nullptr) *error = "Could not allocate task id";
    return false;
  }

  int64_t now = nowSeconds();
  task.id = "task-" + std::to_string(id_reply->integer);
  task.status = "QUEUED";
  task.worker_id = "";
  task.checkpoint = task.range_start;
  task.result = 0;
  task.progress_percent = 0;
  task.error = "";
  task.attempts = 0;
  task.created_at = now;
  task.updated_at = now;
  task.lease_expires_at = 0;

  writeTaskLocked(task);
  enqueueLocked(task.id, task.priority);

  ReplyPtr set_reply(static_cast<redisReply*>(redisCommand(context_, "SADD backplane:tasks %s", task.id.c_str())));
  if (!replyOk(set_reply.get())) {
    if (error != nullptr) *error = "Could not index task";
    return false;
  }

  appendEventLocked("SUBMIT", task.id, "", "queued " + task.name);

  if (task_id != nullptr) *task_id = task.id;
  return true;
}

bool RedisStore::leaseTask(const std::string& worker_id, TaskRecord* task) {
  std::lock_guard<std::mutex> guard(mutex_);

  ReplyPtr top_reply(static_cast<redisReply*>(redisCommand(context_, "ZREVRANGE backplane:pending 0 0")));
  if (top_reply == nullptr || top_reply->type != REDIS_REPLY_ARRAY || top_reply->elements == 0) {
    touchWorkerLocked(worker_id, "");
    return false;
  }

  std::string id = top_reply->element[0]->str == nullptr ? "" : top_reply->element[0]->str;
  ReplyPtr remove_reply(static_cast<redisReply*>(redisCommand(context_, "ZREM backplane:pending %s", id.c_str())));
  if (!replyOk(remove_reply.get())) return false;

  TaskRecord record;
  if (!readTaskLocked(id, &record)) return false;

  record.status = "RUNNING";
  record.worker_id = worker_id;
  record.attempts += 1;
  record.error = "";
  record.updated_at = nowSeconds();
  record.lease_expires_at = record.updated_at + lease_seconds_;
  writeTaskLocked(record);

  ReplyPtr running_reply(static_cast<redisReply*>(redisCommand(
      context_, "ZADD backplane:running %lld %s", static_cast<long long>(record.lease_expires_at), id.c_str())));
  if (!replyOk(running_reply.get())) return false;

  touchWorkerLocked(worker_id, id);
  appendEventLocked("LEASE", id, worker_id, "attempt " + std::to_string(record.attempts));

  if (task != nullptr) *task = record;
  return true;
}

bool RedisStore::heartbeat(const std::string& task_id, const std::string& worker_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!isOwnedByLocked(task_id, worker_id)) return false;

  int64_t lease_deadline = nowSeconds() + lease_seconds_;
  ReplyPtr reply(static_cast<redisReply*>(redisCommand(
      context_, "ZADD backplane:running %lld %s", static_cast<long long>(lease_deadline), task_id.c_str())));
  if (!replyOk(reply.get())) return false;

  ReplyPtr hash_reply(static_cast<redisReply*>(redisCommand(
      context_, "HSET backplane:task:%s updated_at %lld lease_expires_at %lld",
      task_id.c_str(),
      static_cast<long long>(nowSeconds()),
      static_cast<long long>(lease_deadline))));

  touchWorkerLocked(worker_id, task_id);
  return replyOk(hash_reply.get());
}

bool RedisStore::checkpoint(const std::string& task_id,
                            const std::string& worker_id,
                            int64_t checkpoint,
                            int64_t partial_result,
                            int progress_percent) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!isOwnedByLocked(task_id, worker_id)) return false;

  int64_t now = nowSeconds();
  int64_t lease_deadline = now + lease_seconds_;
  progress_percent = std::max(0, std::min(100, progress_percent));

  ReplyPtr reply(static_cast<redisReply*>(redisCommand(
      context_,
      "HSET backplane:task:%s checkpoint %lld result %lld progress_percent %d updated_at %lld lease_expires_at %lld",
      task_id.c_str(),
      static_cast<long long>(checkpoint),
      static_cast<long long>(partial_result),
      progress_percent,
      static_cast<long long>(now),
      static_cast<long long>(lease_deadline))));

  if (!replyOk(reply.get())) return false;

  ReplyPtr running_reply(static_cast<redisReply*>(redisCommand(
      context_, "ZADD backplane:running %lld %s", static_cast<long long>(lease_deadline), task_id.c_str())));
  if (!replyOk(running_reply.get())) return false;

  touchWorkerLocked(worker_id, task_id);
  appendEventLocked("CHECKPOINT", task_id, worker_id, std::to_string(progress_percent) + "%");
  return true;
}

bool RedisStore::completeTask(const std::string& task_id,
                              const std::string& worker_id,
                              bool success,
                              int64_t result,
                              const std::string& error) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!isOwnedByLocked(task_id, worker_id)) return false;

  TaskRecord record;
  if (!readTaskLocked(task_id, &record)) return false;

  ReplyPtr remove_reply(static_cast<redisReply*>(redisCommand(context_, "ZREM backplane:running %s", task_id.c_str())));
  if (!replyOk(remove_reply.get())) return false;

  record.updated_at = nowSeconds();
  record.lease_expires_at = 0;
  clearWorkerTaskLocked(worker_id);

  if (success) {
    record.status = "DONE";
    record.worker_id = "";
    record.result = result;
    record.progress_percent = 100;
    record.error = "";
    writeTaskLocked(record);
    appendEventLocked("DONE", task_id, worker_id, "result " + std::to_string(result));
    return true;
  }

  if (record.attempts < record.max_attempts) {
    record.status = "QUEUED";
    record.worker_id = "";
    record.error = error;
    writeTaskLocked(record);
    enqueueLocked(record.id, record.priority);
    appendEventLocked("RETRY", task_id, worker_id, error);
    return true;
  }

  record.status = "FAILED";
  record.worker_id = "";
  record.error = error;
  writeTaskLocked(record);
  appendEventLocked("FAILED", task_id, worker_id, error);
  return true;
}

bool RedisStore::preemptIfHigherPriority(const std::string& task_id,
                                         const std::string& worker_id,
                                         int* queued_priority) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!isOwnedByLocked(task_id, worker_id)) return false;

  ReplyPtr top_reply(static_cast<redisReply*>(
      redisCommand(context_, "ZREVRANGE backplane:pending 0 0")));
  if (top_reply == nullptr
      || top_reply->type != REDIS_REPLY_ARRAY
      || top_reply->elements == 0
      || top_reply->element[0]->str == nullptr) {
    return false;
  }

  TaskRecord running;
  TaskRecord queued;
  const std::string queued_id = top_reply->element[0]->str;
  if (!readTaskLocked(task_id, &running)
      || !readTaskLocked(queued_id, &queued)
      || !scheduler_policy::shouldPreempt(running.priority, queued.priority)) {
    return false;
  }

  ReplyPtr remove_reply(static_cast<redisReply*>(
      redisCommand(context_, "ZREM backplane:running %s", task_id.c_str())));
  if (!replyOk(remove_reply.get())) return false;

  running.status = "QUEUED";
  running.worker_id = "";
  running.lease_expires_at = 0;
  running.updated_at = nowSeconds();
  running.preemptions += 1;
  running.attempts = std::max(0, running.attempts - 1);
  writeTaskLocked(running);
  enqueueLocked(running.id, running.priority);
  clearWorkerTaskLocked(worker_id);
  appendEventLocked(
      "PREEMPT",
      task_id,
      worker_id,
      "yielded priority " + std::to_string(running.priority)
          + " for priority " + std::to_string(queued.priority));

  if (queued_priority != nullptr) *queued_priority = queued.priority;
  return true;
}

bool RedisStore::getTask(const std::string& task_id, TaskRecord* task) {
  std::lock_guard<std::mutex> guard(mutex_);
  return readTaskLocked(task_id, task);
}

std::vector<TaskRecord> RedisStore::listTasks() {
  std::lock_guard<std::mutex> guard(mutex_);

  std::vector<TaskRecord> tasks;
  ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_, "SMEMBERS backplane:tasks")));
  if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) return tasks;

  for (size_t i = 0; i < reply->elements; i++) {
    if (reply->element[i]->str == nullptr) continue;

    TaskRecord record;
    if (readTaskLocked(reply->element[i]->str, &record)) {
      tasks.push_back(record);
    }
  }

  std::sort(tasks.begin(), tasks.end(), [](const TaskRecord& left, const TaskRecord& right) {
    return left.id < right.id;
  });

  return tasks;
}

std::vector<WorkerRecord> RedisStore::listWorkers() {
  std::lock_guard<std::mutex> guard(mutex_);

  std::vector<WorkerRecord> workers;
  ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_, "SMEMBERS backplane:workers")));
  if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) return workers;

  for (size_t i = 0; i < reply->elements; i++) {
    if (reply->element[i]->str == nullptr) continue;

    std::string worker_id = reply->element[i]->str;
    ReplyPtr worker_reply(static_cast<redisReply*>(redisCommand(context_, "HGETALL backplane:worker:%s", worker_id.c_str())));
    std::map<std::string, std::string> fields = readHash(worker_reply.get());
    if (fields.empty()) continue;

    WorkerRecord worker;
    worker.worker_id = getField(fields, "worker_id", worker_id);
    worker.current_task_id = getField(fields, "current_task_id");
    worker.last_seen_at = toInt64(getField(fields, "last_seen_at"));
    workers.push_back(worker);
  }

  std::sort(workers.begin(), workers.end(), [](const WorkerRecord& left, const WorkerRecord& right) {
    return left.worker_id < right.worker_id;
  });

  return workers;
}

std::vector<EventRecord> RedisStore::listEvents(int limit) {
  std::lock_guard<std::mutex> guard(mutex_);

  if (limit <= 0) limit = 25;
  if (limit > 200) limit = 200;

  std::vector<EventRecord> events;
  ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_, "LRANGE backplane:events 0 %d", limit - 1)));
  if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) return events;

  for (size_t i = 0; i < reply->elements; i++) {
    if (reply->element[i]->str == nullptr) continue;

    std::vector<std::string> parts = splitEvent(reply->element[i]->str);
    if (parts.size() < 6) continue;

    EventRecord event;
    event.id = toInt64(parts[0]);
    event.timestamp = toInt64(parts[1]);
    event.type = parts[2];
    event.task_id = parts[3];
    event.worker_id = parts[4];
    event.message = parts[5];
    events.push_back(event);
  }

  return events;
}

int RedisStore::requeueStaleTasks() {
  std::lock_guard<std::mutex> guard(mutex_);
  int64_t now = nowSeconds();

  ReplyPtr reply(static_cast<redisReply*>(redisCommand(
      context_, "ZRANGEBYSCORE backplane:running -inf %lld", static_cast<long long>(now))));

  if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) return 0;

  int requeued = 0;
  for (size_t i = 0; i < reply->elements; i++) {
    if (reply->element[i]->str == nullptr) continue;

    std::string task_id = reply->element[i]->str;
    TaskRecord record;
    if (!readTaskLocked(task_id, &record)) continue;
    if (record.status != "RUNNING") continue;

    ReplyPtr remove_reply(static_cast<redisReply*>(redisCommand(context_, "ZREM backplane:running %s", task_id.c_str())));
    if (!replyOk(remove_reply.get())) continue;

    std::string old_worker = record.worker_id;
    record.updated_at = now;
    record.lease_expires_at = 0;

    if (record.attempts < record.max_attempts) {
      record.status = "QUEUED";
      record.worker_id = "";
      record.error = "worker lease expired; task returned to queue";
      writeTaskLocked(record);
      enqueueLocked(record.id, record.priority);
      appendEventLocked("REQUEUE", task_id, old_worker, "lease expired");
      requeued += 1;
    } else {
      record.status = "FAILED";
      record.worker_id = "";
      record.error = "worker lease expired; no attempts left";
      writeTaskLocked(record);
      appendEventLocked("FAILED", task_id, old_worker, "lease expired; no attempts left");
    }
  }

  return requeued;
}

bool RedisStore::clearAll(std::string* error) {
  std::lock_guard<std::mutex> guard(mutex_);

  ReplyPtr keys(static_cast<redisReply*>(redisCommand(context_, "KEYS backplane:*")));
  if (keys == nullptr || keys->type != REDIS_REPLY_ARRAY) {
    if (error != nullptr) *error = "Could not scan Redis keys";
    return false;
  }

  for (size_t i = 0; i < keys->elements; i++) {
    if (keys->element[i]->str == nullptr) continue;
    ReplyPtr del_reply(static_cast<redisReply*>(redisCommand(context_, "DEL %s", keys->element[i]->str)));
    if (!replyOk(del_reply.get())) {
      if (error != nullptr) *error = "Could not delete " + std::string(keys->element[i]->str);
      return false;
    }
  }

  appendEventLocked("CLEAR", "", "", "cleared demo state");
  return true;
}

bool RedisStore::readTaskLocked(const std::string& task_id, TaskRecord* task) {
  ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_, "HGETALL backplane:task:%s", task_id.c_str())));
  if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) return false;

  std::map<std::string, std::string> fields = readHash(reply.get());

  TaskRecord record;
  record.id = getField(fields, "id");
  record.name = getField(fields, "name");
  record.kind = toInt(getField(fields, "kind"), 1);
  record.range_start = toInt64(getField(fields, "range_start"));
  record.range_end = toInt64(getField(fields, "range_end"));
  record.chunk_size = toInt(getField(fields, "chunk_size"), 10000);
  record.priority = toInt(getField(fields, "priority"), 1);
  record.max_attempts = toInt(getField(fields, "max_attempts"), 3);
  record.attempts = toInt(getField(fields, "attempts"));
  record.preemptions = toInt(getField(fields, "preemptions"));
  record.status = getField(fields, "status", "QUEUED");
  record.worker_id = getField(fields, "worker_id");
  record.checkpoint = toInt64(getField(fields, "checkpoint"), record.range_start);
  record.result = toInt64(getField(fields, "result"));
  record.progress_percent = toInt(getField(fields, "progress_percent"));
  record.error = getField(fields, "error");
  record.created_at = toInt64(getField(fields, "created_at"));
  record.updated_at = toInt64(getField(fields, "updated_at"));
  record.lease_expires_at = toInt64(getField(fields, "lease_expires_at"));

  if (task != nullptr) *task = record;
  return !record.id.empty();
}

bool RedisStore::isOwnedByLocked(const std::string& task_id, const std::string& worker_id) {
  TaskRecord record;
  if (!readTaskLocked(task_id, &record)) return false;
  return record.status == "RUNNING" && record.worker_id == worker_id;
}

void RedisStore::writeTaskLocked(const TaskRecord& task) {
  ReplyPtr reply(static_cast<redisReply*>(redisCommand(
      context_,
      "HSET backplane:task:%s id %s name %s kind %d range_start %lld range_end %lld chunk_size %d priority %d max_attempts %d attempts %d preemptions %d status %s worker_id %s checkpoint %lld result %lld progress_percent %d error %s created_at %lld updated_at %lld lease_expires_at %lld",
      task.id.c_str(),
      task.id.c_str(),
      task.name.c_str(),
      task.kind,
      static_cast<long long>(task.range_start),
      static_cast<long long>(task.range_end),
      task.chunk_size,
      task.priority,
      task.max_attempts,
      task.attempts,
      task.preemptions,
      task.status.c_str(),
      task.worker_id.c_str(),
      static_cast<long long>(task.checkpoint),
      static_cast<long long>(task.result),
      task.progress_percent,
      task.error.c_str(),
      static_cast<long long>(task.created_at),
      static_cast<long long>(task.updated_at),
      static_cast<long long>(task.lease_expires_at))));

  if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
    std::cerr << "Redis write failed for " << task.id << "\n";
  }
}

void RedisStore::enqueueLocked(const std::string& task_id, int priority) {
  ReplyPtr seq_reply(static_cast<redisReply*>(redisCommand(context_, "INCR backplane:queue_seq")));
  int64_t seq = 0;
  if (seq_reply != nullptr && seq_reply->type == REDIS_REPLY_INTEGER) {
    seq = seq_reply->integer;
  }

  double score = static_cast<double>(priority) * 1000000000.0 - static_cast<double>(seq);
  ReplyPtr reply(static_cast<redisReply*>(redisCommand(context_, "ZADD backplane:pending %f %s", score, task_id.c_str())));

  if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
    std::cerr << "Could not enqueue " << task_id << "\n";
  }
}

void RedisStore::touchWorkerLocked(const std::string& worker_id, const std::string& task_id) {
  if (worker_id.empty()) return;

  int64_t now = nowSeconds();
  ReplyPtr set_reply(static_cast<redisReply*>(redisCommand(context_, "SADD backplane:workers %s", worker_id.c_str())));
  if (!replyOk(set_reply.get())) return;

  ReplyPtr hash_reply(static_cast<redisReply*>(redisCommand(
      context_,
      "HSET backplane:worker:%s worker_id %s current_task_id %s last_seen_at %lld",
      worker_id.c_str(),
      worker_id.c_str(),
      task_id.c_str(),
      static_cast<long long>(now))));

  if (hash_reply == nullptr || hash_reply->type == REDIS_REPLY_ERROR) {
    std::cerr << "Could not update worker " << worker_id << "\n";
  }
}

void RedisStore::clearWorkerTaskLocked(const std::string& worker_id) {
  touchWorkerLocked(worker_id, "");
}

void RedisStore::appendEventLocked(const std::string& type,
                                   const std::string& task_id,
                                   const std::string& worker_id,
                                   const std::string& message) {
  ReplyPtr id_reply(static_cast<redisReply*>(redisCommand(context_, "INCR backplane:next_event_id")));
  int64_t id = 0;
  if (id_reply != nullptr && id_reply->type == REDIS_REPLY_INTEGER) {
    id = id_reply->integer;
  }

  std::ostringstream out;
  out << id << FIELD_SEP
      << nowSeconds() << FIELD_SEP
      << type << FIELD_SEP
      << task_id << FIELD_SEP
      << worker_id << FIELD_SEP
      << message;

  std::string packed = out.str();
  ReplyPtr push_reply(static_cast<redisReply*>(redisCommand(context_, "LPUSH backplane:events %s", packed.c_str())));
  if (!replyOk(push_reply.get())) return;

  ReplyPtr trim_reply(static_cast<redisReply*>(redisCommand(context_, "LTRIM backplane:events 0 199")));
  if (!replyOk(trim_reply.get())) {
    std::cerr << "Could not trim event log\n";
  }
}

int64_t RedisStore::nowSeconds() const {
  auto now = std::chrono::system_clock::now();
  auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  return seconds.time_since_epoch().count();
}
