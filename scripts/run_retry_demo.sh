#!/usr/bin/env bash
set -e

docker compose down -v
docker compose up --build -d redis coordinator dashboard
sleep 3

docker compose run --rm coordinator ./build/backplane_client coordinator:50051 submit retry-demo 1 700000 7 25000 4

echo "First worker will fail normally after two checkpoints. The task should go back to QUEUED."
docker compose run --rm coordinator ./build/backplane_worker coordinator:50051 retry-worker --fail-after 2 --sleep-ms 150 --once

docker compose run --rm coordinator ./build/backplane_client coordinator:50051 list
docker compose run --rm coordinator ./build/backplane_client coordinator:50051 events 10

echo "Now finish it with:"
echo "  docker compose run --rm coordinator ./build/backplane_worker coordinator:50051 worker-good --sleep-ms 100 --once"
