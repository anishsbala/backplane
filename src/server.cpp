#include "redis_store.h"
#include "task_print.h"

#include "backplane.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> keep_running(true);

void handleSignal(int) {
  keep_running = false;
}

class BackplaneServiceImpl final : public backplane::BackplaneService::Service {
public:
  explicit BackplaneServiceImpl(RedisStore* store) : store_(store) {}

  grpc::Status SubmitTask(grpc::ServerContext*,
                          const backplane::SubmitTaskRequest* request,
                          backplane::SubmitTaskReply* reply) override {
    if (request->range_end() < request->range_start()) {
      reply->set_ok(false);
      reply->set_message("range_end must be greater than or equal to range_start");
      return grpc::Status::OK;
    }

    TaskRecord task;
    task.name = request->name().empty() ? "prime-count" : request->name();
    task.kind = request->kind() == backplane::TASK_KIND_UNSPECIFIED ? backplane::PRIME_COUNT : request->kind();
    task.range_start = request->range_start();
    task.range_end = request->range_end();
    task.chunk_size = request->chunk_size() <= 0 ? 10000 : request->chunk_size();
    task.priority = request->priority() <= 0 ? 1 : request->priority();
    task.max_attempts = request->max_attempts() <= 0 ? 3 : request->max_attempts();

    std::string task_id;
    std::string error;
    bool ok = store_->submitTask(task, &task_id, &error);

    reply->set_ok(ok);
    reply->set_task_id(task_id);
    reply->set_message(ok ? "queued" : error);
    return grpc::Status::OK;
  }

  grpc::Status LeaseTask(grpc::ServerContext*,
                         const backplane::LeaseTaskRequest* request,
                         backplane::LeaseTaskReply* reply) override {
    TaskRecord task;
    if (!store_->leaseTask(request->worker_id(), &task)) {
      reply->set_found(false);
      reply->set_message("no queued tasks");
      return grpc::Status::OK;
    }

    reply->set_found(true);
    reply->set_message("leased");
    fillProtoTask(task, reply->mutable_task());
    return grpc::Status::OK;
  }

  grpc::Status Heartbeat(grpc::ServerContext*,
                         const backplane::HeartbeatRequest* request,
                         backplane::BasicReply* reply) override {
    bool ok = store_->heartbeat(request->task_id(), request->worker_id());
    reply->set_ok(ok);
    reply->set_message(ok ? "heartbeat saved" : "task is not owned by this worker");
    return grpc::Status::OK;
  }

  grpc::Status Checkpoint(grpc::ServerContext*,
                          const backplane::CheckpointRequest* request,
                          backplane::BasicReply* reply) override {
    bool ok = store_->checkpoint(request->task_id(),
                                 request->worker_id(),
                                 request->checkpoint(),
                                 request->partial_result(),
                                 request->progress_percent());
    reply->set_ok(ok);
    reply->set_message(ok ? "checkpoint saved" : "checkpoint rejected");
    return grpc::Status::OK;
  }

  grpc::Status CompleteTask(grpc::ServerContext*,
                            const backplane::CompleteTaskRequest* request,
                            backplane::BasicReply* reply) override {
    bool ok = store_->completeTask(request->task_id(),
                                   request->worker_id(),
                                   request->success(),
                                   request->result(),
                                   request->error());
    reply->set_ok(ok);
    reply->set_message(ok ? "task updated" : "completion rejected");
    return grpc::Status::OK;
  }

  grpc::Status GetTask(grpc::ServerContext*,
                       const backplane::GetTaskRequest* request,
                       backplane::GetTaskReply* reply) override {
    TaskRecord task;
    if (!store_->getTask(request->task_id(), &task)) {
      reply->set_found(false);
      reply->set_message("task not found");
      return grpc::Status::OK;
    }

    reply->set_found(true);
    reply->set_message("found");
    fillProtoTask(task, reply->mutable_task());
    return grpc::Status::OK;
  }

  grpc::Status ListTasks(grpc::ServerContext*,
                         const backplane::ListTasksRequest*,
                         backplane::ListTasksReply* reply) override {
    std::vector<TaskRecord> tasks = store_->listTasks();

    for (const TaskRecord& task : tasks) {
      fillProtoTask(task, reply->add_tasks());
    }

    return grpc::Status::OK;
  }

  grpc::Status ListWorkers(grpc::ServerContext*,
                           const backplane::ListWorkersRequest*,
                           backplane::ListWorkersReply* reply) override {
    std::vector<WorkerRecord> workers = store_->listWorkers();

    for (const WorkerRecord& worker : workers) {
      fillProtoWorker(worker, reply->add_workers());
    }

    return grpc::Status::OK;
  }

  grpc::Status ListEvents(grpc::ServerContext*,
                          const backplane::ListEventsRequest* request,
                          backplane::ListEventsReply* reply) override {
    std::vector<EventRecord> events = store_->listEvents(request->limit());

    for (const EventRecord& event : events) {
      fillProtoEvent(event, reply->add_events());
    }

    return grpc::Status::OK;
  }

  grpc::Status Stats(grpc::ServerContext*,
                     const backplane::StatsRequest*,
                     backplane::StatsReply* reply) override {
    std::vector<TaskRecord> tasks = store_->listTasks();
    std::vector<WorkerRecord> workers = store_->listWorkers();

    int progress_sum = 0;
    for (const TaskRecord& task : tasks) {
      progress_sum += task.progress_percent;

      if (task.status == "QUEUED") reply->set_queued_tasks(reply->queued_tasks() + 1);
      else if (task.status == "RUNNING") reply->set_running_tasks(reply->running_tasks() + 1);
      else if (task.status == "DONE") reply->set_done_tasks(reply->done_tasks() + 1);
      else if (task.status == "FAILED") reply->set_failed_tasks(reply->failed_tasks() + 1);
    }

    auto now_time = std::chrono::system_clock::now();
    auto now_seconds = std::chrono::time_point_cast<std::chrono::seconds>(now_time).time_since_epoch().count();
    int active_workers = 0;

    for (const WorkerRecord& worker : workers) {
      if (now_seconds - worker.last_seen_at <= 20) {
        active_workers += 1;
      }
    }

    reply->set_total_tasks(static_cast<int>(tasks.size()));
    reply->set_active_workers(active_workers);
    reply->set_average_progress(tasks.empty() ? 0 : progress_sum / static_cast<int>(tasks.size()));
    return grpc::Status::OK;
  }

  grpc::Status RequeueStale(grpc::ServerContext*,
                            const backplane::RequeueStaleRequest*,
                            backplane::RequeueStaleReply* reply) override {
    reply->set_requeued(store_->requeueStaleTasks());
    return grpc::Status::OK;
  }

  grpc::Status ClearTasks(grpc::ServerContext*,
                          const backplane::ClearTasksRequest*,
                          backplane::BasicReply* reply) override {
    std::string error;
    bool ok = store_->clearAll(&error);
    reply->set_ok(ok);
    reply->set_message(ok ? "cleared" : error);
    return grpc::Status::OK;
  }

private:
  RedisStore* store_;
};

void runWatchdog(RedisStore* store) {
  while (keep_running) {
    int requeued = store->requeueStaleTasks();
    if (requeued > 0) {
      std::cout << "watchdog requeued " << requeued << " task(s)\n";
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string address = argc >= 2 ? argv[1] : "0.0.0.0:50051";
  std::string redis_host = argc >= 3 ? argv[2] : "127.0.0.1";
  int redis_port = argc >= 4 ? std::stoi(argv[3]) : 6379;
  int lease_seconds = argc >= 5 ? std::stoi(argv[4]) : 5;

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  RedisStore store(redis_host, redis_port, lease_seconds);
  std::string error;
  if (!store.connect(&error)) {
    std::cerr << "Could not connect to Redis: " << error << "\n";
    return 1;
  }

  BackplaneServiceImpl service(&store);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (server == nullptr) {
    std::cerr << "Could not start coordinator\n";
    return 1;
  }

  std::thread watchdog(runWatchdog, &store);

  std::cout << "Backplane coordinator listening on " << address << "\n";
  std::cout << "Redis: " << redis_host << ":" << redis_port << "\n";
  std::cout << "Lease timeout: " << lease_seconds << " seconds\n";

  while (keep_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  server->Shutdown();
  watchdog.join();
  return 0;
}
