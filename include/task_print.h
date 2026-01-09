#pragma once

#include "backplane.pb.h"
#include "task_record.h"

#include <string>

backplane::TaskStatus statusFromString(const std::string& status);
std::string statusToString(backplane::TaskStatus status);
void fillProtoTask(const TaskRecord& record, backplane::Task* task);
void fillProtoWorker(const WorkerRecord& record, backplane::WorkerInfo* worker);
void fillProtoEvent(const EventRecord& record, backplane::Event* event);
void printTask(const backplane::Task& task);
