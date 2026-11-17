/* ================================================================
   PowerGuard Pro — Dashboard App Logic
   MQTT topics: meter/A/data, meter/B/data, meter/A/relay, meter/B/relay
================================================================ */

// ── CONFIG ────────────────────────────────────────────────────
const CFG = {
    thresholdAmps: 0.5,
    chartMaxPts: 60,
    maxLogEntries: 50,
};

// ── STATE ─────────────────────────────────────────────────────
const state = {
    A: { voltage: 0, current: 0, power: 0, energy: 0, relay: false, online: false },
    B: { voltage: 0, current: 0, power: 0, energy: 0, relay: false, online: false },
    diff: 0,
    theft: false,
    theftCount: 0,
    startTime: Date.now(),
    demoTimer: null,
    mqttConnected: false,
};

// ── CHART SETUP ───────────────────────────────────────────────
const chartCtx = document.getElementById('liveChart').getContext('2d');
const chart = new Chart(chartCtx, {
    type: 'line',
    data: {
        labels: [],
        datasets: [
            {
                label: 'Current A (Supply)', data: [], borderColor: '#ff6b35',
                backgroundColor: 'rgba(255,107,53,0.08)', tension: 0.4, fill: true,
                pointRadius: 0, borderWidth: 2
            },
            {
                label: 'Current B (Consumer)', data: [], borderColor: '#3b8bff',
                backgroundColor: 'rgba(59,139,255,0.08)', tension: 0.4, fill: true,
                pointRadius: 0, borderWidth: 2
            },
        ]
    },
    options: {
        responsive: true, maintainAspectRatio: false, animation: { duration: 300 },
        scales: {
            x: { display: false },
            y: {
                grid: { color: 'rgba(255,255,255,0.05)' }, ticks: { color: '#5a5d7a', font: { size: 11 } },
                title: { display: true, text: 'Current (A)', color: '#5a5d7a', font: { size: 11 } }
            }
        },
        plugins: {
            legend: { display: false }, tooltip: {
                mode: 'index', intersect: false,
                backgroundColor: 'rgba(7,8,15,0.95)', borderColor: 'rgba(255,255,255,0.08)', borderWidth: 1
            }
        }
    }
});

// ── GAUGE UPDATE ──────────────────────────────────────────────
// Ring gauge: 270° arc. Full circumference at r=62 → 2π×62=389.6
// 270° portion = 389.6 × (270/360) = 292.2 (arc for fill)
// dasharray = "292.2 389.6"  dashoffset = 292.2×(1-pct)
function updateGauge(id, value, min, max) {
    const el = document.getElementById(id);
    if (!el) return;
    const pct = Math.min(1, Math.max(0, (value - min) / (max - min)));
    const arcLen = 292.2;
    const offset = arcLen * (1 - pct);
    el.setAttribute('stroke-dasharray', `${arcLen} 389.6`);
    el.setAttribute('stroke-dashoffset', offset);
}

// ── UPDATE METER UI ───────────────────────────────────────────
function updateMeterUI(m) { // m = 'A' or 'B'
    const d = state[m];
    const V = m === 'A' ? 'voltA' : 'voltB';
    const C = m === 'A' ? 'currA' : 'currB';
    const P = m === 'A' ? 'pwrA' : 'pwrB';
    const E = m === 'A' ? 'engA' : 'engB';
    const G = m === 'A' ? 'ringFillA' : 'ringFillB';
    const L = m === 'A' ? 'lastA' : 'lastB';

    document.getElementById(V).textContent = d.voltage.toFixed(1);
    document.getElementById(C).textContent = d.current.toFixed(3);
    document.getElementById(P).textContent = d.power.toFixed(0);
    document.getElementById(E).textContent = d.energy.toFixed(3);
    document.getElementById(L).textContent = 'Updated: ' + new Date().toLocaleTimeString();

    // Voltage gauge 0-250V
    updateGauge(G, d.voltage, 0, 250);
}

// ── UPDATE THEFT ANALYSIS ─────────────────────────────────────
function updateTheftPanel() {
    const diff = state.A.current - state.B.current;
    state.diff = diff < 0 ? 0 : diff;
    const thresh = CFG.thresholdAmps;
    const wasTheft = state.theft;
    state.theft = state.diff > thresh;

    // Center panel values
    document.getElementById('cmpA').textContent = state.A.current.toFixed(2);
    document.getElementById('cmpB').textContent = state.B.current.toFixed(2);
    document.getElementById('diffNum').textContent = state.diff.toFixed(2) + ' A';

    // Diff bar (0 to 2A range)
    const pct = Math.min(100, (state.diff / 2) * 100);
    const fill = document.getElementById('diffFill');
    fill.style.width = pct + '%';
    fill.className = 'diff-bar-fill' + (state.theft ? ' danger' : '');

    // Threshold line position
    const threshPct = Math.min(100, (thresh / 2) * 100);
    document.getElementById('threshLine').style.left = threshPct + '%';

    // Status badge
    const badge = document.getElementById('statusBadge');
    document.getElementById('statusIcon').textContent = state.theft ? '⚠' : '✔';
    document.getElementById('statusTxt').textContent = state.theft ? 'THEFT DETECTED' : 'NORMAL';
    badge.className = 'status-badge' + (state.theft ? ' theft' : '');

    // Theft banner
    const banner = document.getElementById('theftBanner');
    if (state.theft) {
        banner.classList.add('show');
        document.getElementById('bannerDetail').textContent =
            `Diff: ${state.diff.toFixed(2)} A — Supply (${state.A.current.toFixed(2)}A) > Consumer (${state.B.current.toFixed(2)}A)`;
        if (!wasTheft) {
            state.theftCount++;
            document.getElementById('alertCount').textContent = 'Alert #' + state.theftCount;
            document.getElementById('alertTime').textContent = new Date().toLocaleTimeString();
            document.getElementById('totalAlerts').textContent = state.theftCount;
            addLog(`🚨 THEFT DETECTED — Diff: ${state.diff.toFixed(2)} A (Threshold: ${thresh} A)`, 'theft');
        }
    } else {
        banner.classList.remove('show');
        if (wasTheft) addLog('✔ Status returned to NORMAL', 'normal');
    }
}

// ── UPDATE CHART ──────────────────────────────────────────────
function pushChart() {
    const t = new Date().toLocaleTimeString();
    const d = chart.data;
    d.labels.push(t);
    d.datasets[0].data.push(state.A.current);
    d.datasets[1].data.push(state.B.current);
    if (d.labels.length > CFG.chartMaxPts) {
        d.labels.shift();
        d.datasets.forEach(ds => ds.data.shift());
    }
    chart.update();
}

// ── EVENT LOG ─────────────────────────────────────────────────
function addLog(msg, type = 'info') {
    const body = document.getElementById('logBody');
    const t = new Date().toLocaleTimeString();
    const el = document.createElement('div');
    el.className = `log-entry ${type}`;
    el.textContent = `[${t}] ${msg}`;
    body.insertBefore(el, body.firstChild);
    while (body.children.length > CFG.maxLogEntries) body.removeChild(body.lastChild);
}
function clearLog() {
    document.getElementById('logBody').innerHTML = '';
    addLog('Log cleared', 'info');
}

// ── DEVICE STATUS ─────────────────────────────────────────────
function setOnline(m, on) {
    state[m].online = on;
    const dot = document.getElementById(`dot${m}`);
    const pill = document.getElementById(`pill${m}`);
    if (dot) dot.className = 'online-dot' + (on ? ` on-${m.toLowerCase()}` : '');
    if (pill) pill.className = `device-pill${on ? ` online-${m.toLowerCase()}` : ''}`;
}

// ── RELAY CONTROL ─────────────────────────────────────────────
function toggleRelay(m) {
    if (!state.mqttConnected) { addLog('Not connected to MQTT broker', 'warn'); return; }
    state[m].relay = !state[m].relay;
    const cmd = state[m].relay ? 'ON' : 'OFF';
    mqttClient.publish(`meter/${m}/relay`, cmd);
    updateRelayBtn(m);
    addLog(`Relay ${m} set to ${cmd}`, 'info');
}
function updateRelayBtn(m) {
    const on = state[m].relay;
    const btn = document.getElementById(`relay${m}Btn`);
    const ico = document.getElementById(`relay${m}Icon`);
    const txt = document.getElementById(`relay${m}Txt`);
    if (btn) btn.className = `relay-btn ${m.toLowerCase()}` + (on ? ' relay-on' : '');
    if (ico) ico.textContent = on ? '●' : '○';
    if (txt) txt.textContent = on ? 'ON' : 'OFF';
}

// ── THRESHOLD ─────────────────────────────────────────────────
function updateThreshold(val) {
    CFG.thresholdAmps = parseFloat(val);
    document.getElementById('threshVal').textContent = val + ' A';
    document.getElementById('threshDisp').textContent = parseFloat(val).toFixed(2);
    updateTheftPanel();
}

// ── MQTT ─────────────────────────────────────────────────────
let mqttClient = null;

function mqttConnect() {
    const url = document.getElementById('brokerUrl').value.trim();
    if (!url) return;
    addLog(`Connecting to ${url} …`, 'info');
    document.getElementById('mqttStatus').textContent = '…';

    try {
        mqttClient = mqtt.connect(url, { clientId: 'PowerGuardDash_' + Math.random().toString(16).slice(2) });

        mqttClient.on('connect', () => {
            state.mqttConnected = true;
            document.getElementById('mqttStatus').textContent = '✔';
            document.getElementById('connectBtn').disabled = true;
            document.getElementById('disconnectBtn').disabled = false;
            addLog(`Connected to ${url}`, 'normal');

            mqttClient.subscribe('meter/A/data');
            mqttClient.subscribe('meter/B/data');
            mqttClient.subscribe('meter/A/status');
            mqttClient.subscribe('meter/B/status');
            addLog('Subscribed to meter/A/data and meter/B/data', 'info');
        });

        mqttClient.on('message', handleMessage);

        mqttClient.on('error', err => {
            addLog('MQTT error: ' + err.message, 'warn');
            document.getElementById('mqttStatus').textContent = '✖';
        });

        mqttClient.on('close', () => {
            state.mqttConnected = false;
            document.getElementById('mqttStatus').textContent = '✖';
            document.getElementById('connectBtn').disabled = false;
            document.getElementById('disconnectBtn').disabled = true;
            setOnline('A', false); setOnline('B', false);
            addLog('Disconnected from MQTT broker', 'warn');
        });
    } catch (e) { addLog('Connection failed: ' + e.message, 'warn'); }
}

function mqttDisconnect() {
    if (mqttClient) mqttClient.end();
}

function handleMessage(topic, raw) {
    const msg = raw.toString();
    try {
        if (topic === 'meter/A/data') {
            const d = JSON.parse(msg);
            state.A.voltage = d.voltage || 0;
            state.A.current = d.current || 0;
            state.A.power = d.power || 0;
            state.A.energy = d.energy || 0;
            state.A.relay = d.relay || false;
            updateMeterUI('A');
            updateRelayBtn('A');
            setOnline('A', true);
            updateTheftPanel();
            pushChart();
        } else if (topic === 'meter/B/data') {
            const d = JSON.parse(msg);
            state.B.voltage = d.voltage || 0;
            state.B.current = d.current || 0;
            state.B.power = d.power || 0;
            state.B.energy = d.energy || 0;
            state.B.relay = d.relay || false;
            updateMeterUI('B');
            updateRelayBtn('B');
            setOnline('B', true);
            updateTheftPanel();
        } else if (topic === 'meter/A/status') {
            const d = JSON.parse(msg);
            setOnline('A', d.online !== false);
        } else if (topic === 'meter/B/status') {
            const d = JSON.parse(msg);
            setOnline('B', d.online !== false);
        }
    } catch (e) { addLog('Parse error: ' + e.message, 'warn'); }
}

// ── DEMO SIMULATION ──────────────────────────────────────────
let demoRunning = false;
let demoStep = 0;

function startDemo() {
    if (demoRunning) { stopDemo(); return; }
    demoRunning = true;
    demoStep = 0;
    document.getElementById('demoBtn').textContent = '■ Stop Demo';
    document.getElementById('demoBtn').classList.add('running');
    addLog('▶ Demo simulation started', 'info');

    state.demoTimer = setInterval(() => {
        demoStep++;
        const t = demoStep * 2;
        // Animated values: periodic but with theft event at step 15-25
        const baseV = 230 + Math.sin(t * 0.1) * 3;
        const baseCa = 2.2 + Math.sin(t * 0.2) * 0.3;
        let baseCb = baseCa - 0.1 + Math.sin(t * 0.15) * 0.15;

        // Inject theft scenario between steps 15 and 30
        if (demoStep >= 15 && demoStep <= 30) baseCb = baseCa - 0.8;

        state.A.voltage = parseFloat(baseV.toFixed(2));
        state.A.current = parseFloat(baseCa.toFixed(4));
        state.A.power = parseFloat((baseV * baseCa).toFixed(2));
        state.A.energy = parseFloat((state.A.energy + state.A.power * 2 / 3600000).toFixed(5));

        state.B.voltage = parseFloat((baseV - 0.5).toFixed(2));
        state.B.current = parseFloat(baseCb.toFixed(4));
        state.B.power = parseFloat(((baseV - 0.5) * baseCb).toFixed(2));
        state.B.energy = parseFloat((state.B.energy + state.B.power * 2 / 3600000).toFixed(5));

        updateMeterUI('A'); updateMeterUI('B');
        setOnline('A', true); setOnline('B', true);
        updateTheftPanel(); pushChart();

        if (demoStep >= 60) stopDemo();
    }, 1000);
}

function stopDemo() {
    demoRunning = false;
    clearInterval(state.demoTimer);
    document.getElementById('demoBtn').textContent = '▶ Demo Simulation';
    document.getElementById('demoBtn').classList.remove('running');
    addLog('■ Demo simulation stopped', 'info');
    setOnline('A', false); setOnline('B', false);
}

// ── CLOCK ─────────────────────────────────────────────────────
function updateClock() {
    document.getElementById('clock').textContent = new Date().toLocaleTimeString('en-IN');
    // Uptime
    const sec = Math.floor((Date.now() - state.startTime) / 1000);
    const m = String(Math.floor(sec / 60)).padStart(2, '0');
    const s = String(sec % 60).padStart(2, '0');
    document.getElementById('uptimeDisp').textContent = m + ':' + s;
}
setInterval(updateClock, 1000);
updateClock();

// ── INIT LOG ──────────────────────────────────────────────────
addLog('PowerGuard Pro initialized — enter MQTT broker URL and click Connect', 'info');
addLog('MQTT topics: meter/A/data, meter/B/data | Relay: meter/A/relay, meter/B/relay', 'info');
