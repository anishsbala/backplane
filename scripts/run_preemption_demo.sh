#!/usr/bin/env bash
set -euo pipefail

compose=(docker compose)
client=("${compose[@]}" run --rm coordinator ./build/backplane_client coordinator:50051)
worker=("${compose[@]}" run --rm coordinator ./build/backplane_worker coordinator:50051)

"${compose[@]}" down -v
"${compose[@]}" up --build -d redis coordinator

low_output=$("${client[@]}" submit preemptible-low 1 20000000 2 100000 4)
low_id=$(awk '/task_id:/ {print $2}' <<<"$low_output")
if [[ -z "$low_id" ]]; then
  echo "Could not submit low-priority task"
  exit 1
fi

"${worker[@]}" preemption-worker --sleep-ms 100 --once > /tmp/backplane-low.log 2>&1 &
worker_pid=$!
sleep 1

high_output=$("${client[@]}" submit urgent-high 1 300000 9 25000 3)
high_id=$(awk '/task_id:/ {print $2}' <<<"$high_output")
wait "$worker_pid"

"${worker[@]}" preemption-worker --sleep-ms 10 --once
events=$("${client[@]}" events 50)
low_status=$("${client[@]}" status "$low_id")
high_status=$("${client[@]}" status "$high_id")

grep -q "PREEMPT" <<<"$events"
grep -q "preemptions: 1" <<<"$low_status"
grep -q "status:      DONE" <<<"$high_status"

echo "Preemption verified: $low_id yielded at a checkpoint and $high_id completed first."
echo "$events"
