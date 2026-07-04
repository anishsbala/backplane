#!/usr/bin/env bash
set -e

docker compose down -v
docker compose up --build -d redis coordinator dashboard
sleep 3

docker compose run --rm coordinator ./build/backplane_client coordinator:50051 submit recover-demo 1 1000000 8 25000 4

echo "Dashboard: http://localhost:8080"
echo "Running a worker that will crash after three checkpoints..."
set +e
docker compose run --rm coordinator ./build/backplane_worker coordinator:50051 crash-worker --crash-after 3 --sleep-ms 200
set -e

echo "Waiting for coordinator watchdog to requeue the task..."
sleep 7

docker compose run --rm coordinator ./build/backplane_client coordinator:50051 list
docker compose run --rm coordinator ./build/backplane_client coordinator:50051 events 10

echo "Now run a healthy worker:"
echo "  docker compose run --rm coordinator ./build/backplane_worker coordinator:50051 worker-good --sleep-ms 100"
