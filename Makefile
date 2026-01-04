.PHONY: build up down logs dashboard demo seed worker workers crash fail list stats events clear clean

build:
	docker compose build

up:
	docker compose up --build -d redis coordinator dashboard

dashboard: up
	@echo "Dashboard: http://localhost:8080"

down:
	docker compose down -v

logs:
	docker compose logs -f coordinator dashboard

demo:
	docker compose run --rm coordinator ./build/backplane_client coordinator:50051 demo

seed:
	docker compose run --rm coordinator ./build/backplane_client coordinator:50051 seed

worker:
	docker compose run --rm coordinator ./build/backplane_worker coordinator:50051 worker-1 --sleep-ms 100

workers:
	docker compose --profile workers up --build worker-a worker-b

crash:
	docker compose run --rm coordinator ./build/backplane_worker coordinator:50051 crash-worker --crash-after 3 --sleep-ms 200

fail:
	docker compose run --rm coordinator ./build/backplane_worker coordinator:50051 retry-worker --fail-after 2 --sleep-ms 150 --once

list:
	docker compose run --rm coordinator ./build/backplane_client coordinator:50051 list

stats:
	docker compose run --rm coordinator ./build/backplane_client coordinator:50051 stats

events:
	docker compose run --rm coordinator ./build/backplane_client coordinator:50051 events 25

clear:
	docker compose run --rm coordinator ./build/backplane_client coordinator:50051 clear

clean:
	rm -rf build
