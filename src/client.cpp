#include "task_print.h"

#include "backplane.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>

namespace {

class Client {
public:
  explicit Client(const std::string& address) {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    stub_ = backplane::BackplaneService::NewStub(channel);
  }

  int submit(int argc, char** argv) {
    if (argc < 8) {
      std::cout << "submit needs: <name> <start> <end> <priority> <chunk_size> [max_attempts]\n";
      return 1;
    }

    grpc::ClientContext context;
    backplane::SubmitTaskRequest request;
    backplane::SubmitTaskReply reply;

    request.set_name(argv[3]);
    request.set_kind(backplane::PRIME_COUNT);
    request.set_range_start(std::stoll(argv[4]));
    request.set_range_end(std::stoll(argv[5]));
    request.set_priority(std::stoi(argv[6]));
    request.set_chunk_size(std::stoi(argv[7]));
    request.set_max_attempts(argc >= 9 ? std::stoi(argv[8]) : 3);

    grpc::Status status = stub_->SubmitTask(&context, request, &reply);
    if (!status.ok()) {
      std::cout << "submit failed: " << status.error_message() << "\n";
      return 1;
    }

    std::cout << reply.message() << "\n";
    if (reply.ok()) {
      std::cout << "task_id: " << reply.task_id() << "\n";
    }

    return reply.ok() ? 0 : 1;
  }

  int status(const std::string& task_id) {
    grpc::ClientContext context;
    backplane::GetTaskRequest request;
    backplane::GetTaskReply reply;

    request.set_task_id(task_id);
    grpc::Status status = stub_->GetTask(&context, request, &reply);
    if (!status.ok()) {
      std::cout << "status failed: " << status.error_message() << "\n";
      return 1;
    }

    if (!reply.found()) {
      std::cout << reply.message() << "\n";
      return 1;
    }

    printTask(reply.task());
    return 0;
  }

  int list() {
    grpc::ClientContext context;
    backplane::ListTasksRequest request;
    backplane::ListTasksReply reply;

    grpc::Status status = stub_->ListTasks(&context, request, &reply);
    if (!status.ok()) {
      std::cout << "list failed: " << status.error_message() << "\n";
      return 1;
    }

    for (const backplane::Task& task : reply.tasks()) {
      std::cout << task.id()
                << " | " << statusToString(task.status())
                << " | priority=" << task.priority()
                << " | attempts=" << task.attempts() << "/" << task.max_attempts()
                << " | progress=" << task.progress_percent() << "%"
                << " | checkpoint=" << task.checkpoint()
                << " | result=" << task.result()
                << " | worker=" << (task.worker_id().empty() ? "-" : task.worker_id())
                << "\n";
    }

    if (reply.tasks().empty()) {
      std::cout << "no tasks\n";
    }

    return 0;
  }

  int workers() {
    grpc::ClientContext context;
    backplane::ListWorkersRequest request;
    backplane::ListWorkersReply reply;

    grpc::Status status = stub_->ListWorkers(&context, request, &reply);
    if (!status.ok()) {
      std::cout << "workers failed: " << status.error_message() << "\n";
      return 1;
    }

    for (const backplane::WorkerInfo& worker : reply.workers()) {
      std::cout << worker.worker_id()
                << " | task=" << (worker.current_task_id().empty() ? "-" : worker.current_task_id())
                << " | last_seen=" << worker.last_seen_at()
                << "\n";
    }

    if (reply.workers().empty()) {
      std::cout << "no workers seen yet\n";
    }

    return 0;
  }

  int events(int limit) {
    grpc::ClientContext context;
    backplane::ListEventsRequest request;
    backplane::ListEventsReply reply;

    request.set_limit(limit);
    grpc::Status status = stub_->ListEvents(&context, request, &reply);
    if (!status.ok()) {
      std::cout << "events failed: " << status.error_message() << "\n";
      return 1;
    }

    for (const backplane::Event& event : reply.events()) {
      std::cout << event.id()
                << " | " << event.type()
                << " | task=" << (event.task_id().empty() ? "-" : event.task_id())
                << " | worker=" << (event.worker_id().empty() ? "-" : event.worker_id())
                << " | " << event.message()
                << "\n";
    }

    if (reply.events().empty()) {
      std::cout << "no events\n";
    }

    return 0;
  }

  int stats() {
    grpc::ClientContext context;
    backplane::StatsRequest request;
    backplane::StatsReply reply;

    grpc::Status status = stub_->Stats(&context, request, &reply);
    if (!status.ok()) {
      std::cout << "stats failed: " << status.error_message() << "\n";
      return 1;
    }

    std::cout << "total:   " << reply.total_tasks() << "\n";
    std::cout << "queued:  " << reply.queued_tasks() << "\n";
    std::cout << "running: " << reply.running_tasks() << "\n";
    std::cout << "done:    " << reply.done_tasks() << "\n";
    std::cout << "failed:  " << reply.failed_tasks() << "\n";
    std::cout << "workers: " << reply.active_workers() << "\n";
    std::cout << "average progress: " << reply.average_progress() << "%\n";
    return 0;
  }

  int requeue() {
    grpc::ClientContext context;
    backplane::RequeueStaleRequest request;
    backplane::RequeueStaleReply reply;

    grpc::Status status = stub_->RequeueStale(&context, request, &reply);
    if (!status.ok()) {
      std::cout << "requeue failed: " << status.error_message() << "\n";
      return 1;
    }

    std::cout << "requeued: " << reply.requeued() << "\n";
    return 0;
  }

  int clear() {
    grpc::ClientContext context;
    backplane::ClearTasksRequest request;
    backplane::BasicReply reply;

    grpc::Status status = stub_->ClearTasks(&context, request, &reply);
    if (!status.ok()) {
      std::cout << "clear failed: " << status.error_message() << "\n";
      return 1;
    }

    std::cout << reply.message() << "\n";
    return reply.ok() ? 0 : 1;
  }

  int demo() {
    const char* first[] = {
        "backplane_client", "addr", "submit", "low-priority", "1", "300000", "1", "10000", "3"};
    const char* second[] = {
        "backplane_client", "addr", "submit", "high-priority", "1", "300000", "9", "10000", "3"};
    const char* third[] = {
        "backplane_client", "addr", "submit", "medium-priority", "1", "300000", "5", "10000", "3"};

    submit(9, const_cast<char**>(first));
    submit(9, const_cast<char**>(second));
    submit(9, const_cast<char**>(third));
    return list();
  }

  int seed() {
    const char* one[] = {
        "backplane_client", "addr", "submit", "analytics-refresh", "1", "750000", "8", "25000", "4"};
    const char* two[] = {
        "backplane_client", "addr", "submit", "daily-report", "1", "550000", "4", "20000", "3"};
    const char* three[] = {
        "backplane_client", "addr", "submit", "thumbnail-index", "1", "350000", "6", "15000", "3"};
    const char* four[] = {
        "backplane_client", "addr", "submit", "audit-backfill", "1", "900000", "2", "30000", "5"};

    submit(9, const_cast<char**>(one));
    submit(9, const_cast<char**>(two));
    submit(9, const_cast<char**>(three));
    submit(9, const_cast<char**>(four));
    return list();
  }

private:
  std::unique_ptr<backplane::BackplaneService::Stub> stub_;
};

void usage() {
  std::cout << "Usage:\n";
  std::cout << "  backplane_client <coordinator_addr> submit <name> <start> <end> <priority> <chunk_size> [max_attempts]\n";
  std::cout << "  backplane_client <coordinator_addr> status <task_id>\n";
  std::cout << "  backplane_client <coordinator_addr> list\n";
  std::cout << "  backplane_client <coordinator_addr> workers\n";
  std::cout << "  backplane_client <coordinator_addr> events [limit]\n";
  std::cout << "  backplane_client <coordinator_addr> stats\n";
  std::cout << "  backplane_client <coordinator_addr> requeue\n";
  std::cout << "  backplane_client <coordinator_addr> clear\n";
  std::cout << "  backplane_client <coordinator_addr> demo\n";
  std::cout << "  backplane_client <coordinator_addr> seed\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 1;
  }

  std::string address = argv[1];
  std::string command = argv[2];
  Client client(address);

  if (command == "submit") {
    return client.submit(argc, argv);
  }

  if (command == "status") {
    if (argc < 4) {
      usage();
      return 1;
    }

    return client.status(argv[3]);
  }

  if (command == "list") {
    return client.list();
  }

  if (command == "workers") {
    return client.workers();
  }

  if (command == "events") {
    int limit = argc >= 4 ? std::stoi(argv[3]) : 25;
    return client.events(limit);
  }

  if (command == "stats") {
    return client.stats();
  }

  if (command == "requeue") {
    return client.requeue();
  }

  if (command == "clear") {
    return client.clear();
  }

  if (command == "demo") {
    return client.demo();
  }

  if (command == "seed") {
    return client.seed();
  }

  usage();
  return 1;
}
