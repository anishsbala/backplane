const taskRows = document.getElementById('taskRows');
const workerList = document.getElementById('workerList');
const eventList = document.getElementById('eventList');
const lastRefresh = document.getElementById('lastRefresh');
const toast = document.getElementById('toast');

const stats = {
  total: document.getElementById('statTotal'),
  queued: document.getElementById('statQueued'),
  running: document.getElementById('statRunning'),
  done: document.getElementById('statDone'),
  failed: document.getElementById('statFailed'),
  workers: document.getElementById('statWorkers')
};

function showToast(message) {
  toast.textContent = message;
  toast.classList.remove('hidden');
  window.setTimeout(() => toast.classList.add('hidden'), 2800);
}

function statusName(status) {
  if (!status) return 'UNKNOWN';
  return String(status).replace('TASK_STATUS_', '');
}

function niceTime(seconds) {
  const value = Number(seconds || 0);
  if (value <= 0) return 'never';
  return new Date(value * 1000).toLocaleTimeString();
}

function escapeHtml(value) {
  return String(value ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

async function request(path, options = {}) {
  const response = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...options
  });

  const data = await response.json();
  if (!response.ok) {
    throw new Error(data.error || 'request failed');
  }

  return data;
}

function renderStats(data) {
  const stat = data.stats || {};
  stats.total.textContent = stat.total_tasks || 0;
  stats.queued.textContent = stat.queued_tasks || 0;
  stats.running.textContent = stat.running_tasks || 0;
  stats.done.textContent = stat.done_tasks || 0;
  stats.failed.textContent = stat.failed_tasks || 0;
  stats.workers.textContent = stat.active_workers || 0;
}

function renderTasks(tasks) {
  if (!tasks || tasks.length === 0) {
    taskRows.innerHTML = '<tr><td colspan="9" class="muted">No tasks yet. Seed the demo or submit one manually.</td></tr>';
    return;
  }

  taskRows.innerHTML = tasks.map((task) => {
    const status = statusName(task.status);
    const progress = Number(task.progress_percent || 0);
    const worker = task.worker_id || '-';

    return `
      <tr>
        <td>${escapeHtml(task.id)}</td>
        <td>${escapeHtml(task.name)}</td>
        <td><span class="badge ${status}">${status}</span></td>
        <td>${escapeHtml(task.priority)}</td>
        <td>${escapeHtml(task.attempts)}/${escapeHtml(task.max_attempts)}</td>
        <td>${escapeHtml(task.preemptions || 0)}</td>
        <td>
          <div class="progress-shell"><div class="progress-fill" style="width: ${progress}%"></div></div>
          <span class="progress-label">${progress}%</span>
        </td>
        <td>${escapeHtml(task.checkpoint)}</td>
        <td>${escapeHtml(task.result)}</td>
        <td>${escapeHtml(worker)}</td>
      </tr>
    `;
  }).join('');
}

function renderWorkers(workers) {
  if (!workers || workers.length === 0) {
    workerList.innerHTML = '<div class="worker"><span>No workers have checked in yet.</span></div>';
    return;
  }

  workerList.innerHTML = workers.map((worker) => {
    const task = worker.current_task_id || 'idle';
    return `
      <div class="worker">
        <strong>${escapeHtml(worker.worker_id)}</strong>
        <span>task: ${escapeHtml(task)} · last seen: ${niceTime(worker.last_seen_at)}</span>
      </div>
    `;
  }).join('');
}

function renderEvents(events) {
  if (!events || events.length === 0) {
    eventList.innerHTML = '<div class="event"><span>No events yet.</span></div>';
    return;
  }

  eventList.innerHTML = events.slice(0, 12).map((event) => {
    const parts = [];
    if (event.task_id) parts.push(event.task_id);
    if (event.worker_id) parts.push(event.worker_id);

    return `
      <div class="event">
        <strong>${escapeHtml(event.type)} <span>${escapeHtml(parts.join(' · '))}</span></strong>
        <span>${escapeHtml(event.message)} · ${niceTime(event.timestamp)}</span>
      </div>
    `;
  }).join('');
}

async function loadState() {
  try {
    const data = await request('/api/state');
    renderStats(data);
    renderTasks(data.tasks);
    renderWorkers(data.workers);
    renderEvents(data.events);
    lastRefresh.textContent = `updated ${new Date().toLocaleTimeString()}`;
  } catch (error) {
    showToast(error.message);
  }
}

document.getElementById('taskForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = new FormData(event.target);
  const body = Object.fromEntries(form.entries());

  try {
    const reply = await request('/api/tasks', {
      method: 'POST',
      body: JSON.stringify(body)
    });

    showToast(reply.ok ? `queued ${reply.task_id}` : reply.message);
    await loadState();
  } catch (error) {
    showToast(error.message);
  }
});

document.getElementById('seedButton').addEventListener('click', async () => {
  try {
    await request('/api/demo', { method: 'POST', body: '{}' });
    showToast('demo tasks queued');
    await loadState();
  } catch (error) {
    showToast(error.message);
  }
});

document.getElementById('requeueButton').addEventListener('click', async () => {
  try {
    const reply = await request('/api/requeue', { method: 'POST', body: '{}' });
    showToast(`requeued ${reply.requeued || 0} task(s)`);
    await loadState();
  } catch (error) {
    showToast(error.message);
  }
});

document.getElementById('clearButton').addEventListener('click', async () => {
  const ok = window.confirm('Clear all Backplane demo state from Redis?');
  if (!ok) return;

  try {
    await request('/api/clear', { method: 'POST', body: '{}' });
    showToast('cleared');
    await loadState();
  } catch (error) {
    showToast(error.message);
  }
});

loadState();
window.setInterval(loadState, 1500);
