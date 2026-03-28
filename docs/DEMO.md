# Demo Script

Use this when showing the project in an interview or recording a quick demo.

## 1. Start the system

```bash
docker compose up --build -d redis coordinator dashboard
```

Open:

```text
http://localhost:8080
```

## 2. Seed tasks

Click **Seed demo tasks** in the dashboard.

Point out:

- tasks are `QUEUED`,
- each task has priority and chunk size,
- the event log shows `SUBMIT` events.

## 3. Start workers

```bash
docker compose --profile workers up worker-a worker-b
```

Point out:

- workers lease tasks over gRPC,
- high priority tasks are picked first,
- checkpoints update Redis after every chunk,
- the dashboard is reading coordinator state live.

## 4. Priority preemption

```bash
./scripts/run_preemption_demo.sh
```

Point out:

- the low-priority task yields only after saving a checkpoint,
- the urgent task is leased and completed first,
- the event stream records a `PREEMPT` transition.

## 5. Crash recovery

```bash
./scripts/run_crash_demo.sh
```

Point out:

- the worker crashes after a few checkpoints,
- the task does not restart from zero,
- the coordinator requeues it after the lease expires,
- another worker resumes from the saved checkpoint.

## 6. Measured recovery

```bash
./scripts/run_recovery_benchmark.sh
```

Point out:

- the worker crashes after completing 38 of 40 chunks,
- 95% progress is read back from Redis,
- a replacement worker finishes from the saved checkpoint.

## 7. Retry handling

```bash
./scripts/run_retry_demo.sh
```

Point out:

- failed work is retried when attempts remain,
- `attempts` increments on each lease,
- task state is still stored in Redis.
