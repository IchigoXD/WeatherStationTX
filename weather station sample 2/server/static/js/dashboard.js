/**
 * ============================================================================
 *  Weather Station Network — Dashboard JavaScript
 * ============================================================================
 *  Features:
 *    - Real-time data polling (5-second intervals)
 *    - Per-node and network overview views
 *    - Temperature & Humidity time-series chart
 *    - Soil Temperature depth profile bar chart
 *    - Network average comparison chart
 *    - Rain gauge chart (parent node)
 *    - Node status indicators (online/offline)
 *    - Security alert monitoring
 *    - Health check trigger
 *    - Date range filtering (DS-01: now functional)
 *    - Recent transmission log table
 *    - AbortController for request dedup (DS-06)
 * ============================================================================
 */

/* ── Chart.js Global Configuration ────────────────────────────────────── */
Chart.defaults.color = '#9fa8da';
Chart.defaults.borderColor = 'rgba(255, 255, 255, 0.04)';
Chart.defaults.font.family = "'Inter', sans-serif";
Chart.defaults.font.size = 11;
Chart.defaults.plugins.legend.labels.usePointStyle = true;
Chart.defaults.plugins.legend.labels.pointStyleWidth = 10;
Chart.defaults.plugins.legend.labels.padding = 16;
Chart.defaults.plugins.tooltip.backgroundColor = 'rgba(10, 16, 41, 0.95)';
Chart.defaults.plugins.tooltip.borderColor = 'rgba(0, 229, 255, 0.2)';
Chart.defaults.plugins.tooltip.borderWidth = 1;
Chart.defaults.plugins.tooltip.cornerRadius = 8;
Chart.defaults.plugins.tooltip.padding = 10;

/* ── State ─────────────────────────────────────────────────────────────── */
let currentNode = null;
let charts = {};
let refreshInterval = null;
let hoursRange = 24;

/* DS-06: AbortController to cancel in-flight requests before each cycle */
let currentAbortController = null;

/* ── Color Palette (per-node) ──────────────────────────────────────────── */
const NODE_COLORS = {
    1: { line: '#00e5ff', fill: 'rgba(0, 229, 255, 0.1)',  label: 'Child 01' },
    2: { line: '#7c4dff', fill: 'rgba(124, 77, 255, 0.1)', label: 'Child 02' },
    3: { line: '#00e676', fill: 'rgba(0, 230, 118, 0.1)',  label: 'Child 03' },
    4: { line: '#ffab00', fill: 'rgba(255, 171, 0, 0.1)',  label: 'Child 04' },
    5: { line: '#ff1744', fill: 'rgba(255, 23, 68, 0.1)',  label: 'Parent' },
};

/* ── Pretty Label Map ──────────────────────────────────────────────────── */
const FIELD_LABELS = {
    temp: 'Temperature',
    humidity: 'Humidity',
    dew_point: 'Dew Point',
    leaf_wetness: 'Leaf Wetness',
    rain_gauge: 'Rain',
};

/* ── Initialization ────────────────────────────────────────────────────── */
document.addEventListener('DOMContentLoaded', () => {
    initCharts();
    updateAll();
    refreshInterval = setInterval(updateAll, 5000);

    // Hours range selector
    const hoursSelect = document.getElementById('hours-range');
    if (hoursSelect) {
        hoursSelect.addEventListener('change', (e) => {
            hoursRange = parseInt(e.target.value);
            updateAll();
        });
    }

    // Mobile hamburger toggle
    const hamburger = document.getElementById('hamburger-toggle');
    if (hamburger) {
        hamburger.addEventListener('click', () => {
            document.getElementById('sidebar').classList.toggle('sidebar-open');
        });
    }
});

/* ── Navigation (DS-02: event passed explicitly) ──────────────────────── */
function switchNode(nodeId, evt) {
    currentNode = nodeId;

    // Update active link
    document.querySelectorAll('.nav-link').forEach(l => l.classList.remove('active'));
    if (evt && evt.currentTarget) {
        evt.currentTarget.classList.add('active');
    }

    // Update title
    const title = document.getElementById('view-title');
    const subtitle = document.getElementById('view-subtitle');

    if (nodeId === null) {
        title.textContent = 'Network Overview';
        subtitle.textContent = 'Real-time telemetry from all 5 stations';
    } else {
        const info = NODE_COLORS[nodeId] || { label: `Station ${nodeId}` };
        title.textContent = `${info.label} — Telemetry`;
        subtitle.textContent = `Detailed sensor data for station ${String(nodeId).padStart(2, '0')}`;
    }

    // Close mobile sidebar after selection
    document.getElementById('sidebar').classList.remove('sidebar-open');

    updateAll();
}

/* ── Health Check ──────────────────────────────────────────────────────── */
async function triggerHealthCheck() {
    const btn = document.getElementById('btn-health');
    btn.textContent = 'CHECKING...';
    btn.disabled = true;

    try {
        const resp = await fetch('/api/alive_check');
        const data = await resp.json();
        btn.textContent = 'QUEUED ✓';
        setTimeout(() => {
            btn.textContent = 'HEALTH CHECK';
            btn.disabled = false;
        }, 5000);
    } catch (e) {
        btn.textContent = 'FAILED';
        setTimeout(() => {
            btn.textContent = 'HEALTH CHECK';
            btn.disabled = false;
        }, 3000);
    }
}

/* ── Master Update ─────────────────────────────────────────────────────── */
async function updateAll() {
    /* DS-06: Abort any in-flight requests from a previous cycle */
    if (currentAbortController) {
        currentAbortController.abort();
    }
    currentAbortController = new AbortController();
    const signal = currentAbortController.signal;

    try {
        await Promise.all([
            updateNodeStatus(signal),
            updateStats(signal),
            updateSensorChart('temp', charts.temp, 'Temperature (°C)', '#00e5ff', signal),
            updateSensorChart('humidity', charts.humidity, 'Humidity (%)', '#7c4dff', signal),
            updateSensorChart('dew_point', charts.dew, 'Dew Point (°C)', '#00e676', signal),
            updateSensorChart('leaf_wetness', charts.leaf, 'Leaf Wetness', '#ffab00', signal),
            updateSoilChart(signal),
            updateRainChart(signal),
            updateLogs(signal),
        ]);
    } catch (e) {
        if (e.name !== 'AbortError') {
            console.error('Update error:', e);
        }
    }
}

/* ── Node Status ───────────────────────────────────────────────────────── */
async function updateNodeStatus(signal) {
    try {
        const resp = await fetch('/api/node_status', { signal });
        const nodes = await resp.json();

        // Update sidebar status dots
        nodes.forEach(node => {
            const dot = document.getElementById(`status-dot-${node.node_id}`);
            if (dot) {
                dot.className = `status-dot ${node.online ? 'online' : 'offline'}`;
            }
        });

        // Update node status cards
        const grid = document.getElementById('nodes-grid');
        if (!grid) return;

        grid.innerHTML = nodes.map(node => `
            <div class="glass node-card">
                <div class="node-indicator ${node.online ? 'online' : 'offline'}">
                    ${String(node.node_id).padStart(2, '0')}
                </div>
                <div class="node-info">
                    <h4>${node.name || 'Station ' + node.node_id}</h4>
                    <p>${node.online ? 'Online' : 'Offline'} · ${
                        node.last_seen ? timeSince(node.last_seen) : 'Never seen'
                    }</p>
                    ${node.online ? `
                        <div class="node-signal">
                            <span class="signal-icon">📶</span>
                            <span class="signal-text">${node.rssi ? node.rssi + ' dBm' : '—'}</span>
                        </div>
                    ` : ''}
                </div>
            </div>
        `).join('');

        // Update security badge
        const hasAlert = nodes.some(n => n.security_alert);
        updateSecurityBadge(hasAlert);

    } catch (e) {
        if (e.name !== 'AbortError') console.error('Node status error:', e);
    }
}

/* ── Stats Cards ───────────────────────────────────────────────────────── */
async function updateStats(signal) {
    try {
        let url = '/api/latest';
        const resp = await fetch(url, { signal });
        const data = await resp.json();

        if (!data || data.length === 0) {
            document.getElementById('stats-grid').innerHTML = emptyState('📡', 'No Data', 'Waiting for station transmissions...');
            return;
        }

        let displayData;
        if (currentNode !== null) {
            displayData = data.find(d => d.node_id === currentNode);
            if (!displayData) {
                document.getElementById('stats-grid').innerHTML = emptyState('📡', 'No Data', 'No readings from this station yet.');
                return;
            }
        } else {
            // Average of all latest readings
            displayData = {
                temp: avg(data, 'temp'),
                humidity: avg(data, 'humidity'),
                dew_point: avg(data, 'dew_point'),
                leaf_wetness: avg(data, 'leaf_wetness'),
                rain_gauge: sum(data, 'rain_gauge'),
                soil_temp_0: avg(data, 'soil_temp_0'),
            };
        }

        const grid = document.getElementById('stats-grid');
        grid.innerHTML = `
            <div class="glass stat-card">
                <div class="stat-label">Temperature</div>
                <div class="stat-value">${formatNum(displayData.temp, 1)}°C</div>
                <div class="stat-subtext">${currentNode ? 'Local' : 'Network avg'}</div>
            </div>
            <div class="glass stat-card">
                <div class="stat-label">Humidity</div>
                <div class="stat-value">${formatNum(displayData.humidity, 1)}%</div>
                <div class="stat-subtext">Relative humidity</div>
            </div>
            <div class="glass stat-card">
                <div class="stat-label">Dew Point</div>
                <div class="stat-value text-primary">${formatNum(displayData.dew_point, 1)}°C</div>
                <div class="stat-subtext">Calculated</div>
            </div>
            <div class="glass stat-card">
                <div class="stat-label">Leaf Wetness</div>
                <div class="stat-value">${displayData.leaf_wetness || 0}</div>
                <div class="stat-subtext">ADC value</div>
            </div>
            <div class="glass stat-card">
                <div class="stat-label">Rain Gauge</div>
                <div class="stat-value">${displayData.rain_gauge || 0}</div>
                <div class="stat-subtext">${currentNode && currentNode <= 4 ? 'From Parent' : 'Local tips'}</div>
            </div>
            <div class="glass stat-card">
                <div class="stat-label">Ground Temp</div>
                <div class="stat-value">${formatNum(displayData.soil_temp_0, 1)}°C</div>
                <div class="stat-subtext">Surface level</div>
            </div>
        `;
    } catch (e) {
        if (e.name !== 'AbortError') console.error('Stats error:', e);
    }
}

/* ── Generic Sensor Chart Update ───────────────────────────────────────── */
async function updateSensorChart(field, chartInstance, label, color, signal) {
    try {
        /* DS-01: Include hoursRange in all chart API calls */
        const nodeParam = currentNode !== null ? `&node_id=${currentNode}` : '';
        const resp = await fetch(`/api/time_series?field=${field}&hours=${hoursRange}${nodeParam}`, { signal });
        const data = await resp.json();

        if (!data || data.length === 0) {
            chartInstance.data.datasets = [];
            chartInstance.update();
            return;
        }

        /* DS-05: Use pretty labels instead of raw field names */
        const prettyField = FIELD_LABELS[field] || field;

        if (currentNode !== null) {
            // Single Node View
            chartInstance.data.labels = data.map(d => formatTime(d.reading_time));
            chartInstance.data.datasets = [{
                label: label,
                data: data.map(d => d[field]),
                borderColor: color,
                backgroundColor: color + '15',
                fill: true,
                tension: 0.4,
                pointRadius: 1,
            }];
        } else {
            // Network Overview View (Multiple lines)
            const byNode = groupByNode(data);
            const datasets = [];

            for (const [nodeId, readings] of Object.entries(byNode)) {
                const nc = NODE_COLORS[nodeId] || { line: '#999', label: `Node ${nodeId}` };
                datasets.push({
                    label: `${nc.label} ${prettyField}`,
                    data: readings.map(r => ({ x: r.reading_time, y: r[field] })),
                    borderColor: nc.line,
                    backgroundColor: 'transparent',
                    fill: false,
                    tension: 0.4,
                    pointRadius: 1,
                });
            }

            // Align labels to the unique timestamps
            const allTimes = [...new Set(data.map(r => r.reading_time))].sort();
            chartInstance.data.labels = allTimes.map(t => formatTime(t));
            chartInstance.data.datasets = datasets.map(ds => ({
                ...ds,
                data: allTimes.map(t => {
                    const match = ds.data.find(d => d.x === t);
                    return match ? match.y : null;
                })
            }));
        }

        chartInstance.update('none');
    } catch (e) {
        if (e.name !== 'AbortError') console.error(`${field} chart error:`, e);
    }
}

/* ── Soil Temperature Depth Profile ────────────────────────────────────── */
async function updateSoilChart(signal) {
    try {
        /* DS-03: In network overview, fetch latest per node and average soil temps */
        const nodeParam = currentNode !== null ? `&node_id=${currentNode}` : '';
        const resp = await fetch(`/api/readings?limit=1&hours=${hoursRange}${nodeParam}`, { signal });
        const data = await resp.json();

        if (!data || data.length === 0) return;

        let latest;
        if (currentNode !== null) {
            latest = data[0];
        } else {
            /* DS-03: Fetch latest readings from all nodes and average soil temps */
            const latestResp = await fetch(`/api/latest`, { signal });
            const latestData = await latestResp.json();
            if (!latestData || latestData.length === 0) return;

            latest = {
                soil_temp_0:  avg(latestData, 'soil_temp_0'),
                soil_temp_20: avg(latestData, 'soil_temp_20'),
                soil_temp_40: avg(latestData, 'soil_temp_40'),
                soil_temp_60: avg(latestData, 'soil_temp_60'),
                soil_temp_80: avg(latestData, 'soil_temp_80'),
            };
        }

        const depths = ['0m', '20m', '40m', '60m', '80m'];
        const values = [
            latest.soil_temp_0, latest.soil_temp_20, latest.soil_temp_40,
            latest.soil_temp_60, latest.soil_temp_80
        ];

        charts.soil.data.labels = depths;
        charts.soil.data.datasets = [{
            label: currentNode ? 'Soil Temperature (°C)' : 'Network Avg Soil Temp (°C)',
            data: values,
            backgroundColor: [
                'rgba(0, 229, 255, 0.7)',
                'rgba(0, 229, 255, 0.55)',
                'rgba(0, 229, 255, 0.4)',
                'rgba(0, 229, 255, 0.3)',
                'rgba(0, 229, 255, 0.2)',
            ],
            borderColor: '#00e5ff',
            borderWidth: 1,
            borderRadius: 6,
        }];

        charts.soil.update('none');
    } catch (e) {
        if (e.name !== 'AbortError') console.error('Soil chart error:', e);
    }
}

/* ── Rain Gauge Chart ──────────────────────────────────────────────────── */
async function updateRainChart(signal) {
    try {
        // Requirement: Rain (from parent) for child stations
        // If currentNode is 1-4, we fetch from node_id=5
        let fetchId = currentNode || 5;
        const titleEl = document.getElementById('rain-chart-title');

        if (currentNode >= 1 && currentNode <= 4) {
            fetchId = 5;
            if (titleEl) titleEl.innerHTML = '<span class="icon">🌧️</span> Rain Gauge (from Parent)';
        } else if (currentNode === 5) {
            if (titleEl) titleEl.innerHTML = '<span class="icon">🌧️</span> Rain Gauge (Local)';
        } else {
            // DS-04: Network view or Main Station — default title
            if (titleEl) titleEl.innerHTML = '<span class="icon">🌧️</span> Rain Gauge';
        }

        /* DS-01: Include hoursRange */
        const resp = await fetch(`/api/time_series?field=rain_gauge&hours=${hoursRange}&node_id=${fetchId}`, { signal });
        const data = await resp.json();

        if (!data || data.length === 0) {
            charts.rain.data.datasets = [];
            charts.rain.update();
            return;
        }

        charts.rain.data.labels = data.map(d => formatTime(d.reading_time));
        charts.rain.data.datasets = [{
            label: 'Rain (tips)',
            data: data.map(d => d.rain_gauge || 0),
            backgroundColor: 'rgba(124, 77, 255, 0.4)',
            borderColor: '#7c4dff',
            borderWidth: 1,
            borderRadius: 4,
        }];

        charts.rain.update('none');
    } catch (e) {
        if (e.name !== 'AbortError') console.error('Rain chart error:', e);
    }
}


/* ── Transmission Logs Table ───────────────────────────────────────────── */
async function updateLogs(signal) {
    try {
        /* DS-01: Include hoursRange in log fetch */
        const nodeParam = currentNode !== null ? `?node_id=${currentNode}&limit=15&hours=${hoursRange}` : `?limit=15&hours=${hoursRange}`;
        const resp = await fetch(`/api/readings${nodeParam}`, { signal });
        const data = await resp.json();

        const tbody = document.getElementById('log-body');
        if (!tbody) return;

        if (!data || data.length === 0) {
            tbody.innerHTML = `<tr><td colspan="8" style="text-align: center; color: var(--text-muted); padding: 2rem;">No transmission logs yet.</td></tr>`;
            return;
        }

        tbody.innerHTML = data.map(d => {
            const sig = getSignalInfo(d.rssi);
            return `
                <tr>
                    <td class="node-name">Station ${String(d.node_id).padStart(2, '0')}</td>
                    <td>${d.reading_time || '—'}</td>
                    <td>${formatNum(d.temp, 1)}°C</td>
                    <td>${formatNum(d.humidity, 1)}%</td>
                    <td>${d.leaf_wetness || 0}</td>
                    <td>
                        <span class="signal-tag" title="SNR: ${d.snr}dB">
                            ${d.rssi ? d.rssi + ' dBm' : '—'}
                            <small>${sig.label}</small>
                        </span>
                    </td>
                    <td><span class="tag tag-success">OK</span></td>
                    <td>${d.security_alert
                        ? '<span class="tag tag-danger">ALERT</span>'
                        : '<span class="tag tag-muted">None</span>'
                    }</td>
                </tr>
            `;
        }).join('');
    } catch (e) {
        if (e.name !== 'AbortError') console.error('Logs error:', e);
    }
}

/* ── Chart Initialization ──────────────────────────────────────────────── */
function initCharts() {
    const chartDefaultOptions = {
        responsive: true,
        maintainAspectRatio: false,
        animation: { duration: 400 },
        plugins: {
            legend: { position: 'top', labels: { boxWidth: 12, padding: 15 } },
        },
        scales: {
            x: {
                grid: { color: 'rgba(255,255,255,0.03)' },
                ticks: { maxRotation: 45, maxTicksLimit: 8 },
            },
            y: {
                grid: { color: 'rgba(255,255,255,0.03)' },
                beginAtZero: false,
            },
        },
    };

    // 1. Temperature
    charts.temp = new Chart(document.getElementById('chart-temp').getContext('2d'), {
        type: 'line',
        data: { labels: [], datasets: [] },
        options: { ...chartDefaultOptions, plugins: { ...chartDefaultOptions.plugins, title: { display: false } } }
    });

    // 2. Humidity
    charts.humidity = new Chart(document.getElementById('chart-humidity').getContext('2d'), {
        type: 'line',
        data: { labels: [], datasets: [] },
        options: { ...chartDefaultOptions, plugins: { ...chartDefaultOptions.plugins, title: { display: false } } }
    });

    // 3. Dew Point
    charts.dew = new Chart(document.getElementById('chart-dew').getContext('2d'), {
        type: 'line',
        data: { labels: [], datasets: [] },
        options: { ...chartDefaultOptions, plugins: { ...chartDefaultOptions.plugins, title: { display: false } } }
    });

    // 4. Soil Temperature Profile
    charts.soil = new Chart(document.getElementById('chart-soil').getContext('2d'), {
        type: 'bar',
        data: { labels: [], datasets: [] },
        options: {
            ...chartDefaultOptions,
            indexAxis: 'y',
            scales: {
                x: { grid: { color: 'rgba(255,255,255,0.03)' }, title: { display: true, text: '°C' } },
                y: { grid: { color: 'rgba(255,255,255,0.03)' } }
            }
        }
    });

    // 5. Leaf Wetness
    charts.leaf = new Chart(document.getElementById('chart-leaf').getContext('2d'), {
        type: 'line',
        data: { labels: [], datasets: [] },
        options: { ...chartDefaultOptions, scales: { ...chartDefaultOptions.scales, y: { ...chartDefaultOptions.scales.y, beginAtZero: true } } }
    });

    // 6. Rain Gauge
    charts.rain = new Chart(document.getElementById('chart-rain').getContext('2d'), {
        type: 'bar',
        data: { labels: [], datasets: [] },
        options: {
            ...chartDefaultOptions,
            scales: {
                y: { beginAtZero: true, title: { display: true, text: 'Tips' } }
            }
        }
    });
}

/* ── Utility Functions ─────────────────────────────────────────────────── */

function formatNum(val, decimals = 1) {
    if (val === null || val === undefined) return '—';
    return Number(val).toFixed(decimals);
}

function formatTime(dateStr) {
    if (!dateStr) return '';
    const d = new Date(dateStr);
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function timeSince(dateStr) {
    const d = new Date(dateStr);
    const diff = Math.floor((Date.now() - d.getTime()) / 1000);
    if (diff < 60) return `${diff}s ago`;
    if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
    if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`;
    return `${Math.floor(diff / 86400)}d ago`;
}

function avg(arr, field) {
    const valid = arr.filter(d => d[field] != null);
    if (valid.length === 0) return 0;
    return valid.reduce((s, d) => s + d[field], 0) / valid.length;
}

function sum(arr, field) {
    return arr.reduce((s, d) => s + (d[field] || 0), 0);
}

function groupByNode(items) {
    const groups = {};
    items.forEach(item => {
        const id = item.node_id;
        if (!groups[id]) groups[id] = [];
        groups[id].push(item);
    });
    return groups;
}

function emptyState(icon, title, msg) {
    return `
        <div class="empty-state" style="grid-column: 1 / -1;">
            <div class="icon">${icon}</div>
            <h3>${title}</h3>
            <p>${msg}</p>
        </div>
    `;
}

function updateSecurityBadge(hasAlert) {
    const badge = document.getElementById('security-badge');
    if (!badge) return;
    if (hasAlert) {
        badge.className = 'badge badge-alert';
        badge.innerHTML = '<span class="dot"></span> SECURITY COMPROMISED';
    } else {
        badge.className = 'badge badge-secure';
        badge.innerHTML = '<span class="dot"></span> SYSTEM SECURE';
    }
}

function getSignalInfo(rssi) {
    if (rssi === null || rssi === 0) return { label: '—', class: 'none' };
    if (rssi >= -70) return { label: 'Excellent', class: 'best' };
    if (rssi >= -85) return { label: 'Good', class: 'good' };
    if (rssi >= -100) return { label: 'Fair', class: 'fair' };
    return { label: 'Weak', class: 'poor' };
}
