const path = require('path');
const express = require('express');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');

const app = express();
const port = process.env.PORT || 8080;
const coordinator = process.env.COORDINATOR_ADDR || 'coordinator:50051';
const protoPath = path.join(__dirname, 'proto', 'backplane.proto');

const packageDefinition = protoLoader.loadSync(protoPath, {
  keepCase: true,
  longs: String,
  enums: String,
  defaults: true,
  oneofs: true
});

const loaded = grpc.loadPackageDefinition(packageDefinition);
const client = new loaded.backplane.BackplaneService(coordinator, grpc.credentials.createInsecure());

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

function call(method, request) {
  const name = client[method] ? method : method.charAt(0).toLowerCase() + method.slice(1);

  return new Promise((resolve, reject) => {
    if (!client[name]) {
      reject(new Error('missing gRPC method ' + method));
      return;
    }

    client[name](request, (error, reply) => {
      if (error) {
        reject(error);
        return;
      }

      resolve(reply);
    });
  });
}

function cleanInt(value, fallback) {
  const parsed = Number.parseInt(value, 10);
  if (Number.isNaN(parsed)) return fallback;
  return parsed;
}

app.get('/api/state', async (req, res) => {
  try {
    const [stats, tasks, workers, events] = await Promise.all([
      call('Stats', {}),
      call('ListTasks', {}),
      call('ListWorkers', {}),
      call('ListEvents', { limit: 50 })
    ]);

    res.json({
      stats,
      tasks: tasks.tasks || [],
      workers: workers.workers || [],
      events: events.events || []
    });
  } catch (error) {
    res.status(502).json({ error: error.message });
  }
});

app.post('/api/tasks', async (req, res) => {
  try {
    const body = req.body || {};
    const request = {
      name: String(body.name || 'prime-count'),
      kind: 'PRIME_COUNT',
      range_start: String(cleanInt(body.range_start, 1)),
      range_end: String(cleanInt(body.range_end, 300000)),
      priority: cleanInt(body.priority, 5),
      chunk_size: cleanInt(body.chunk_size, 10000),
      max_attempts: cleanInt(body.max_attempts, 3)
    };

    const reply = await call('SubmitTask', request);
    res.json(reply);
  } catch (error) {
    res.status(502).json({ error: error.message });
  }
});

app.post('/api/demo', async (req, res) => {
  try {
    const tasks = [
      { name: 'ui-high-priority', range_start: '1', range_end: '650000', priority: 9, chunk_size: 25000, max_attempts: 4 },
      { name: 'ui-medium-priority', range_start: '1', range_end: '450000', priority: 5, chunk_size: 20000, max_attempts: 3 },
      { name: 'ui-low-priority', range_start: '1', range_end: '300000', priority: 2, chunk_size: 15000, max_attempts: 3 }
    ];

    const replies = [];
    for (const task of tasks) {
      replies.push(await call('SubmitTask', { ...task, kind: 'PRIME_COUNT' }));
    }

    res.json({ ok: true, replies });
  } catch (error) {
    res.status(502).json({ error: error.message });
  }
});

app.post('/api/requeue', async (req, res) => {
  try {
    const reply = await call('RequeueStale', {});
    res.json(reply);
  } catch (error) {
    res.status(502).json({ error: error.message });
  }
});

app.post('/api/clear', async (req, res) => {
  try {
    const reply = await call('ClearTasks', {});
    res.json(reply);
  } catch (error) {
    res.status(502).json({ error: error.message });
  }
});

app.listen(port, () => {
  console.log('Backplane dashboard listening on ' + port);
  console.log('Coordinator: ' + coordinator);
});
