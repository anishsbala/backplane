#!/usr/bin/env bash
set -e

docker compose down -v
docker compose up --build -d redis coordinator dashboard
sleep 3

docker compose run --rm coordinator ./build/backplane_client coordinator:50051 demo

echo "Dashboard: http://localhost:8080"
echo "Start two workers with:"
echo "  docker compose --profile workers up worker-a worker-b"
