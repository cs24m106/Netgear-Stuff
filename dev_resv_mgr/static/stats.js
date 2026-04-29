/* Operations statistics page — Chart.js + /api/stats/* */

const charts = {};

function queryFromForm() {
    const f = document.getElementById('statsFilterForm');
    const fd = new FormData(f);
    const p = new URLSearchParams();
    for (const [k, v] of fd.entries()) {
        if (k === 'recent_limit') continue; /* used only for events API, not summary segment */
        if (v != null && String(v).trim() !== '') p.set(k, String(v).trim());
    }
    return p;
}

function readRecentLimitFromForm() {
    const el = document.getElementById('recentLimitInput');
    const v = parseInt(el && el.value, 10);
    return Math.min(500, Math.max(1, v || 100));
}

function destroyChart(id) {
    if (charts[id]) {
        charts[id].destroy();
        charts[id] = null;
    }
}

function doughnut(id, labels, data, title) {
    destroyChart(id);
    const el = document.getElementById(id);
    if (!el) return;
    charts[id] = new Chart(el, {
        type: 'doughnut',
        data: {
            labels,
            datasets: [{ data, backgroundColor: ['#6f42c1', '#0d6efd', '#20c997', '#fd7e14', '#dc3545', '#6c757d', '#6610f2', '#198754'] }],
        },
        options: {
            plugins: { title: { display: !!title, text: title || '' }, legend: { position: 'bottom' } },
        },
    });
}

function barLine(id, labels, data, title) {
    destroyChart(id);
    const el = document.getElementById(id);
    if (!el) return;
    charts[id] = new Chart(el, {
        type: 'bar',
        data: {
            labels,
            datasets: [{ label: 'Events', data, backgroundColor: 'rgba(111,66,193,0.55)' }],
        },
        options: {
            responsive: true,
            plugins: { title: { display: true, text: title } },
            scales: { y: { beginAtZero: true, ticks: { precision: 0 } } },
        },
    });
}

let eventsOffset = 0;
let recentLimit = 100;

async function fetchSummary() {
    const p = queryFromForm();
    const url = `/api/stats/summary?${p.toString()}`;
    const r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) throw new Error(`summary ${r.status}`);
    return r.json();
}

async function fetchEvents() {
    const p = queryFromForm();
    recentLimit = readRecentLimitFromForm();
    p.set('events_offset', String(eventsOffset));
    p.set('recent_limit', String(recentLimit));
    const url = `/api/stats/events?${p.toString()}`;
    const r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) throw new Error(`events ${r.status}`);
    return r.json();
}

function renderMeta(meta) {
    const el = document.getElementById('statsMeta');
    if (!el) return;
    el.textContent = `File lines: ${meta.lines_read ?? '—'} · segment physical: ${meta.segment_physical_lines ?? '—'} · matched: ${meta.lines_matched ?? '—'} · parse errors: ${meta.parse_errors ?? 0} · segment [${meta.segment_start}, ${meta.segment_end}) · prev=${meta.has_prev} next=${meta.has_next}`;
}

function renderEvents(events) {
    const tb = document.getElementById('eventsBody');
    if (!tb) return;
    tb.textContent = '';
    for (const row of events) {
        const ch = row.changes || {};
        const tr = document.createElement('tr');
        const cells = [
            row.timestamp || '',
            row.operation || '',
            row.device_id || '',
            row.user || '',
            ch.level || '',
            (ch.message || '').slice(0, 240),
        ];
        cells.forEach((text, i) => {
            const td = document.createElement('td');
            td.className = 'small' + (i === 0 ? ' text-nowrap' : '') + (i === 5 ? ' text-break' : '');
            td.textContent = text;
            tr.appendChild(td);
        });
        tb.appendChild(tr);
    }
}

function renderCharts(data) {
    const byOp = data.by_operation || {};
    doughnut('chartByOp', Object.keys(byOp), Object.values(byOp), 'By operation');

    const byLv = data.by_level || {};
    doughnut('chartByLevel', Object.keys(byLv), Object.values(byLv), 'By level');

    const http = data.http_status || {};
    doughnut('chartHttp', Object.keys(http), Object.values(http), 'HTTP status (response)');

    const tl = data.timeline || [];
    barLine('chartTimeline', tl.map((x) => x.day), tl.map((x) => x.count), 'Events per day (UTC, matched rows)');
}

async function loadAll() {
    const sum = await fetchSummary();
    renderMeta(sum.meta || {});
    renderCharts(sum);
    const ev = await fetchEvents();
    renderMeta(ev.meta || sum.meta || {});
    renderEvents(ev.events || []);
    const m = ev.meta || {};
    document.getElementById('eventsPrev').disabled = (m.events_offset || 0) <= 0;
    document.getElementById('eventsNext').disabled = !m.has_more_events;
    document.getElementById('segPrev').disabled = !sum.meta?.has_prev;
    document.getElementById('segNext').disabled = !sum.meta?.has_next;
}

function exportUrl(kind) {
    const p = queryFromForm();
    const path = kind === 'jsonl' ? '/api/stats/export.jsonl' : '/api/stats/export.csv';
    return `${path}?${p.toString()}`;
}

document.getElementById('statsFilterForm').addEventListener('submit', (e) => {
    e.preventDefault();
    eventsOffset = 0;
    loadAll().catch((err) => console.error(err));
});

document.getElementById('statsRefresh').addEventListener('click', () => {
    loadAll().catch((err) => console.error(err));
});

document.getElementById('segPrev').addEventListener('click', () => {
    const s = document.getElementById('segment');
    s.value = String(parseInt(s.value, 10) + 1 || 1);
    eventsOffset = 0;
    loadAll().catch((err) => console.error(err));
});

document.getElementById('segNext').addEventListener('click', () => {
    const s = document.getElementById('segment');
    const v = Math.max(0, (parseInt(s.value, 10) || 0) - 1);
    s.value = String(v);
    eventsOffset = 0;
    loadAll().catch((err) => console.error(err));
});

document.getElementById('eventsPrev').addEventListener('click', () => {
    eventsOffset = Math.max(0, eventsOffset - recentLimit);
    fetchEvents()
        .then((ev) => {
            renderEvents(ev.events || []);
            const m = ev.meta || {};
            document.getElementById('eventsPrev').disabled = (m.events_offset || 0) <= 0;
            document.getElementById('eventsNext').disabled = !m.has_more_events;
        })
        .catch(console.error);
});

document.getElementById('eventsNext').addEventListener('click', () => {
    eventsOffset += recentLimit;
    fetchEvents()
        .then((ev) => {
            renderEvents(ev.events || []);
            const m = ev.meta || {};
            document.getElementById('eventsPrev').disabled = (m.events_offset || 0) <= 0;
            document.getElementById('eventsNext').disabled = !m.has_more_events;
        })
        .catch(console.error);
});

document.getElementById('exportJsonl').addEventListener('click', (e) => {
    e.preventDefault();
    window.location.href = exportUrl('jsonl');
});

document.getElementById('exportCsv').addEventListener('click', (e) => {
    e.preventDefault();
    window.location.href = exportUrl('csv');
});

window.addEventListener('load', () => {
    recentLimit = 100;
    loadAll().catch((err) => console.error(err));
});
