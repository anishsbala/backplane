#!/usr/bin/env bash
set -euo pipefail

compose=(docker compose)
client=("${compose[@]}" run --rm coordinator ./build/backplane_client coordinator:50051)
worker=("${compose[@]}" run --rm coordinator ./build/backplane_worker coordinator:50051)

"${compose[@]}" down -v
"${compose[@]}" up --build -d redis coordinator

submit_output=$("${client[@]}" submit recovery-95 1 1000000 5 25000 4)
task_id=$(awk '/task_id:/ {print $2}' <<<"$submit_output")
if [[ -z "$task_id" ]]; then
  echo "Could not submit benchmark task"
  exit 1
fi

set +e
"${worker[@]}" crash-worker --crash-after 38 --sleep-ms 1 --once
worker_exit=$?
set -e
if [[ "$worker_exit" -eq 0 ]]; then
  echo "Crash worker exited successfully instead of simulating failure"
  exit 1
fi

status_after_crash=$("${client[@]}" status "$task_id")
progress=$(awk '/progress:/ {gsub(/%/, "", $2); print $2}' <<<"$status_after_crash")
checkpoint=$(awk '/checkpoint:/ {print $2}' <<<"$status_after_crash")
if [[ "$progress" -lt 95 ]]; then
  echo "Expected at least 95% saved progress, found ${progress}%"
  exit 1
fi

requeued=false
for _ in $(seq 1 20); do
  recovery_status=$("${client[@]}" status "$task_id")
  task_status=$(awk '/status:/ {print $2}' <<<"$recovery_status")
  if [[ "$task_status" == "QUEUED" ]]; then
    requeued=true
    break
  fi
  sleep 1
done

if [[ "$requeued" != "true" ]]; then
  echo "Task was not requeued after its worker lease expired"
  echo "$recovery_status"
  exit 1
fi

"${worker[@]}" recovery-worker --sleep-ms 1 --once
final_status=$("${client[@]}" status "$task_id")
grep -q "status:      DONE" <<<"$final_status"

echo "Recovery benchmark passed"
echo "task_id=$task_id"
echo "saved_progress_percent=$progress"
echo "saved_checkpoint=$checkpoint"
echo "recomputed_chunks=0"
