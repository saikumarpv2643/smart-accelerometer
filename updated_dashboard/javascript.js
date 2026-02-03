// ================= BLE UUIDs (Custom GATT Service) =================
const ACCEL_SERVICE_UUID = "12340000-1234-5678-9abc-def012345678";
const ACCEL_DATA_CHAR_UUID = "12340001-1234-5678-9abc-def012345678"; // NOTIFY

let device, accelDataChar, timeChart, fftChart;

// ===== Sensor Parameters =====
const SAMPLE_RATE = 1000;   // 1000 Hz sensor sampling
const LSB_PER_G = 2048.0;   // ±16g range
let sampleCount = 0;
let lastSampleCounter = 0;
let droppedSamples = 0;

// ===== Data Storage =====
let receivedData = [];  // [{t, x, y, z, ts}, ...]

// ===== Latency Tracking =====
let latencyHistory = [];
const LATENCY_WINDOW = 100;
let latencyMax = 0;
let startTime = 0;
let lastBurstId = -1;

// Timestamp Unwrapping
let lastFwTs = 0;
let fwTsWrapOffset = 0;

// ===== Zoom / Window Parameters =====
const WINDOW_MIN = 0.1;
const WINDOW_MAX = 5.0;
const WINDOW_DEFAULT = 1.0;
let windowSeconds = WINDOW_DEFAULT;

// ===== FFT Parameters =====
let fftSize = 512;
let fftAxis = 'x';

// ===== DOM Element References =====
let connectButton, startButton, stopButton, exportButton;
let updateYAxisButton, zoomInButton, zoomOutButton;
let xValue, yValue, zValue;
let yAxisMin, yAxisMax, windowDisplay;
let fftAxisSelect, fftSizeSelect, peakFreq, freqResolution;
let latencyCurrent, latencyAvg, latencyMaxEl;
let sampleCountEl, sampleRateEl, droppedCountEl;

// ================= INIT =================
document.addEventListener("DOMContentLoaded", () => {
    // Get all DOM elements
    connectButton = document.getElementById("connectButton");
    startButton = document.getElementById("startButton");
    stopButton = document.getElementById("stopButton");
    exportButton = document.getElementById("exportButton");
    updateYAxisButton = document.getElementById("updateYAxisButton");
    zoomInButton = document.getElementById("zoomInButton");
    zoomOutButton = document.getElementById("zoomOutButton");
    xValue = document.getElementById("xValue");
    yValue = document.getElementById("yValue");
    zValue = document.getElementById("zValue");
    yAxisMin = document.getElementById("yAxisMin");
    yAxisMax = document.getElementById("yAxisMax");
    windowDisplay = document.getElementById("windowDisplay");

    // FFT elements
    fftAxisSelect = document.getElementById("fftAxisSelect");
    fftSizeSelect = document.getElementById("fftSizeSelect");
    peakFreq = document.getElementById("peakFreq");
    freqResolution = document.getElementById("freqResolution");

    // Latency elements
    latencyCurrent = document.getElementById("latencyCurrent");
    latencyAvg = document.getElementById("latencyAvg");
    latencyMaxEl = document.getElementById("latencyMax");

    // Stats elements
    sampleCountEl = document.getElementById("sampleCount");
    sampleRateEl = document.getElementById("sampleRate");
    droppedCountEl = document.getElementById("droppedCount");

    initCharts();
    updateWindowDisplay();
    updateFftResolution();

    connectButton.onclick = connectBLE;
    startButton.onclick = sendStart;
    stopButton.onclick = sendStop;
    exportButton.onclick = saveDataToCSV;
    updateYAxisButton.onclick = updateYAxis;
    zoomInButton.onclick = zoomIn;
    zoomOutButton.onclick = zoomOut;

    fftAxisSelect.onchange = () => { fftAxis = fftAxisSelect.value; };
    fftSizeSelect.onchange = () => {
        fftSize = parseInt(fftSizeSelect.value);
        updateFftResolution();
    };
});

// ================= BLE =================
async function connectBLE() {
    try {
        device = await navigator.bluetooth.requestDevice({
            filters: [{ name: "ISRO_AccelSensor" }],
            optionalServices: [ACCEL_SERVICE_UUID]
        });

        const server = await device.gatt.connect();
        const service = await server.getPrimaryService(ACCEL_SERVICE_UUID);
        accelDataChar = await service.getCharacteristic(ACCEL_DATA_CHAR_UUID);

        connectButton.textContent = "Connected";
        connectButton.disabled = true;
        startButton.disabled = false;

        console.log("Connected to ISRO_AccelSensor");
    } catch (err) {
        console.error("BLE Error:", err);
        alert("Connection failed: " + err.message);
    }
}

async function sendStart() {
    receivedData = [];
    sampleCount = 0;
    lastSampleCounter = 0;
    droppedSamples = 0;
    latencyHistory = [];
    latencyMax = 0;
    startTime = Date.now();
    window.deviceTimeOffset = undefined;  // Reset clock sync
    window.firstDeviceTs = undefined;     // For relative latency

    // Reset timestamp unwrapping
    lastFwTs = 0;
    fwTsWrapOffset = 0;

    // Clear charts
    timeChart.data.datasets.forEach(ds => ds.data = []);
    fftChart.data.datasets[0].data = [];
    timeChart.update('none');
    fftChart.update('none');

    await accelDataChar.startNotifications();
    accelDataChar.addEventListener("characteristicvaluechanged", onData);
    console.log("Notifications ENABLED");

    startButton.disabled = true;
    stopButton.disabled = false;
    exportButton.disabled = true;
}

async function sendStop() {
    await accelDataChar.stopNotifications();

    stopButton.disabled = true;
    startButton.disabled = false;
    exportButton.disabled = false;

    console.log("Stopped. Samples:", receivedData.length);
}

// ================= DATA PROCESSING (Rev 3 Format) =================
// Packet: burst_id(1) + samples[24×10] + crc16(2) = 243 bytes
// Sample: sample_counter(2) + rel_timestamp_ms(2) + x(2) + y(2) + z(2) = 10 bytes

const SAMPLE_SIZE = 10;
const SAMPLES_PER_PACKET = 24;
const PACKET_SIZE = 243;  // 1 + 240 + 2

function onData(event) {
    const view = event.target.value;
    const receiveTime = Date.now();

    const packetLen = view.byteLength;

    // Determine packet format: Rev 3 (243 bytes) or legacy (141 bytes)
    let samplesInPacket, sampleSize, hasNewFormat;

    if (packetLen >= 243) {
        // Rev 3: 243-byte packet (24 samples × 10 bytes)
        hasNewFormat = true;
        samplesInPacket = SAMPLES_PER_PACKET;
        sampleSize = SAMPLE_SIZE;
    } else if (packetLen >= 15) {
        // Legacy: batch_count(1) + samples[n × 14]
        hasNewFormat = false;
        samplesInPacket = view.getUint8(0);
        sampleSize = 14;
        if (samplesInPacket === 0 || samplesInPacket > 10) {
            console.warn("Invalid legacy batch count:", samplesInPacket);
            return;
        }
    } else {
        console.warn("Unknown packet format, length:", packetLen);
        return;
    }

    // Parse each sample
    for (let i = 0; i < samplesInPacket; i++) {
        let offset, sampleCounter, timestampMs, rawX, rawY, rawZ;

        if (hasNewFormat) {
            // Rev 3 format: burst_id(1) + samples[24 × 10]
            offset = 1 + (i * SAMPLE_SIZE);
            sampleCounter = view.getUint16(offset, true);      // 2 bytes
            timestampMs = view.getUint16(offset + 2, true);    // 2 bytes (relative)
            rawX = view.getInt16(offset + 4, true);
            rawY = view.getInt16(offset + 6, true);
            rawZ = view.getInt16(offset + 8, true);
        } else {
            // Legacy format: batch_count(1) + samples[n × 14]
            offset = 1 + (i * sampleSize);
            sampleCounter = view.getUint32(offset, true);      // 4 bytes
            timestampMs = view.getUint32(offset + 4, true);    // 4 bytes (absolute)
            rawX = view.getInt16(offset + 8, true);
            rawY = view.getInt16(offset + 10, true);
            rawZ = view.getInt16(offset + 12, true);
        }

        // Unwrap 16-bit timestamp (Rev 3+ uses uint16 timestamps that wrap every ~65s)
        if (hasNewFormat) {
            // If timestamp jumped backwards significantly, it wrapped
            if (timestampMs < lastFwTs - 30000) {
                fwTsWrapOffset += 65536;
            }
            lastFwTs = timestampMs;
            timestampMs += fwTsWrapOffset;
        }

        // Check for dropped samples (handle 16-bit wrap for Rev 3)
        if (lastSampleCounter > 0) {
            let expected = (lastSampleCounter + 1) & (hasNewFormat ? 0xFFFF : 0xFFFFFFFF);
            if (sampleCounter !== expected && sampleCounter > lastSampleCounter) {
                const dropped = sampleCounter - lastSampleCounter - 1;
                droppedSamples += dropped;
            }
        }
        lastSampleCounter = sampleCounter;

        // Convert raw counts to g
        const ax_g = rawX / LSB_PER_G;
        const ay_g = rawY / LSB_PER_G;
        const az_g = rawZ / LSB_PER_G;

        // Calculate time from sample counter
        const t = (sampleCount) / SAMPLE_RATE;
        sampleCount++;

        // Store for CSV
        receivedData.push({ t, x: ax_g, y: ay_g, z: az_g, ts: timestampMs });

        // Update chart datasets
        timeChart.data.datasets[0].data.push({ x: t, y: ax_g });
        timeChart.data.datasets[1].data.push({ x: t, y: ay_g });
        timeChart.data.datasets[2].data.push({ x: t, y: az_g });

        // Calculate E2E latency (for burst mode, this includes buffering time)
        // Reset anchor on new burst (detected by ID change)
        let currentBurstId = hasNewFormat ? view.getUint8(0) : -1;
        if (sampleCount === 1 || (hasNewFormat && currentBurstId !== lastBurstId)) {
            window.firstDeviceTs = timestampMs;
            window.firstBrowserTs = receiveTime;
            lastBurstId = currentBurstId;
        }

        // Calculate latency on last sample of packet
        if (i === samplesInPacket - 1 && window.firstDeviceTs !== undefined) {
            const deviceElapsed = timestampMs - window.firstDeviceTs;
            const browserElapsed = receiveTime - window.firstBrowserTs;
            const latency = browserElapsed - deviceElapsed;

            if (latency >= 0) {
                latencyHistory.push(latency);
                if (latencyHistory.length > LATENCY_WINDOW) latencyHistory.shift();
                if (latency > latencyMax) latencyMax = latency;
            }
        }
    }

    const last = receivedData[receivedData.length - 1];
    const lastT = last.t;

    // Update acceleration display
    xValue.textContent = last.x.toFixed(3);
    yValue.textContent = last.y.toFixed(3);
    zValue.textContent = last.z.toFixed(3);

    // Update latency display
    if (latencyHistory.length > 0) {
        const currentLatency = latencyHistory[latencyHistory.length - 1];
        const avgLatency = latencyHistory.reduce((a, b) => a + b, 0) / latencyHistory.length;

        const formatLatency = (ms) => {
            if (ms >= 1000) return (ms / 1000).toFixed(2) + " s";
            return ms.toFixed(0) + " ms";
        };

        latencyCurrent.textContent = formatLatency(currentLatency);
        latencyAvg.textContent = formatLatency(avgLatency);
        latencyMaxEl.textContent = formatLatency(latencyMax);
    }

    // Update statistics
    // Update statistics
    sampleCountEl.textContent = sampleCount.toLocaleString();
    const elapsedSec = (Date.now() - startTime) / 1000;
    if (elapsedSec > 0.5) {
        sampleRateEl.textContent = (sampleCount / elapsedSec).toFixed(1) + " Hz";
    }
    droppedCountEl.textContent = droppedSamples.toString();
}

// Render Loop (Decoupled from Data Rate)
function renderLoop() {
    if (!lastSampleCounter) { // Check if we have data
        requestAnimationFrame(renderLoop);
        return;
    }

    // Trim time-domain chart
    const maxPoints = Math.ceil(windowSeconds * SAMPLE_RATE) + 20;
    timeChart.data.datasets.forEach(ds => {
        while (ds.data.length > maxPoints) ds.data.shift();
    });

    // Update X-axis rolling window
    const lastT = receivedData.length > 0 ? receivedData[receivedData.length - 1].t : 0;
    timeChart.options.scales.x.min = Math.max(0, lastT - windowSeconds);
    timeChart.options.scales.x.max = lastT;

    timeChart.update('none');

    // Check FFT update
    if (sampleCount % 50 === 0 && receivedData.length >= fftSize) {
        computeAndDisplayFFT();
    }

    requestAnimationFrame(renderLoop);
}
requestAnimationFrame(renderLoop);

// ================= FFT =================
function computeAndDisplayFFT() {
    const n = fftSize;
    if (receivedData.length < n) return;

    // Get last n samples for selected axis
    const samples = receivedData.slice(-n).map(d => d[fftAxis]);

    // Apply Hanning window
    const windowed = samples.map((v, i) => {
        const w = 0.5 * (1 - Math.cos(2 * Math.PI * i / (n - 1)));
        return v * w;
    });

    // Compute FFT (simple DFT for clarity)
    const real = new Float32Array(n);
    const imag = new Float32Array(n);

    // Use iterative Cooley-Tukey FFT
    fftCooleyTukey(windowed, real, imag);

    // Compute magnitude spectrum (single-sided)
    const magnitudes = [];
    const freqBinSize = SAMPLE_RATE / n;
    let peakMag = 0;
    let peakIdx = 0;

    for (let k = 1; k < n / 2; k++) {
        const freq = k * freqBinSize;
        const mag = Math.sqrt(real[k] * real[k] + imag[k] * imag[k]) * 2 / n;
        magnitudes.push({ x: freq, y: mag });

        if (mag > peakMag) {
            peakMag = mag;
            peakIdx = k;
        }
    }

    // Update FFT chart
    fftChart.data.datasets[0].data = magnitudes;
    fftChart.data.datasets[0].borderColor =
        fftAxis === 'x' ? '#ef4444' : (fftAxis === 'y' ? '#10b981' : '#3b82f6');
    fftChart.data.datasets[0].label = fftAxis.toUpperCase() + ' FFT';
    fftChart.update('none');

    // Update peak frequency display
    const peakFrequency = peakIdx * freqBinSize;
    peakFreq.textContent = `Peak: ${peakFrequency.toFixed(1)} Hz`;
}

// Cooley-Tukey FFT implementation
function fftCooleyTukey(input, real, imag) {
    const n = input.length;
    const bits = Math.log2(n);

    // Bit-reversal permutation
    for (let i = 0; i < n; i++) {
        const j = reverseBits(i, bits);
        real[j] = input[i];
        imag[j] = 0;
    }

    // Cooley-Tukey iterative FFT
    for (let s = 1; s <= bits; s++) {
        const m = 1 << s;
        const wm_real = Math.cos(-2 * Math.PI / m);
        const wm_imag = Math.sin(-2 * Math.PI / m);

        for (let k = 0; k < n; k += m) {
            let w_real = 1;
            let w_imag = 0;

            for (let j = 0; j < m / 2; j++) {
                const t_real = w_real * real[k + j + m / 2] - w_imag * imag[k + j + m / 2];
                const t_imag = w_real * imag[k + j + m / 2] + w_imag * real[k + j + m / 2];

                const u_real = real[k + j];
                const u_imag = imag[k + j];

                real[k + j] = u_real + t_real;
                imag[k + j] = u_imag + t_imag;
                real[k + j + m / 2] = u_real - t_real;
                imag[k + j + m / 2] = u_imag - t_imag;

                const new_w_real = w_real * wm_real - w_imag * wm_imag;
                const new_w_imag = w_real * wm_imag + w_imag * wm_real;
                w_real = new_w_real;
                w_imag = new_w_imag;
            }
        }
    }
}

function reverseBits(n, bits) {
    let result = 0;
    for (let i = 0; i < bits; i++) {
        result = (result << 1) | (n & 1);
        n >>= 1;
    }
    return result;
}

function updateFftResolution() {
    const resolution = SAMPLE_RATE / fftSize;
    freqResolution.textContent = `Resolution: ${resolution.toFixed(2)} Hz`;
}

// ================= CHARTS =================
function initCharts() {
    const timeCtx = document.getElementById("timeChart").getContext("2d");
    timeChart = new Chart(timeCtx, {
        type: "line",
        data: {
            datasets: [
                { label: "X (g)", borderColor: "#ef4444", backgroundColor: "rgba(239,68,68,0.1)", data: [], borderWidth: 1.5, pointRadius: 0, tension: 0 },
                { label: "Y (g)", borderColor: "#10b981", backgroundColor: "rgba(16,185,129,0.1)", data: [], borderWidth: 1.5, pointRadius: 0, tension: 0 },
                { label: "Z (g)", borderColor: "#3b82f6", backgroundColor: "rgba(59,130,246,0.1)", data: [], borderWidth: 1.5, pointRadius: 0, tension: 0 }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,
            interaction: { intersect: false, mode: 'index' },
            plugins: {
                legend: { position: "top", labels: { boxWidth: 12, padding: 15, font: { size: 11 } } }
            },
            scales: {
                x: { type: "linear", title: { display: true, text: "Time (s)", font: { size: 11 } }, min: 0, max: windowSeconds, grid: { color: '#f0f0f0' } },
                y: { title: { display: true, text: "Acceleration (g)", font: { size: 11 } }, min: -2, max: 2, grid: { color: '#f0f0f0' } }
            }
        }
    });

    const fftCtx = document.getElementById("fftChart").getContext("2d");
    fftChart = new Chart(fftCtx, {
        type: "line",
        data: {
            datasets: [{
                label: "X FFT",
                borderColor: "#ef4444",
                backgroundColor: "rgba(239,68,68,0.2)",
                data: [],
                borderWidth: 1.5,
                pointRadius: 0,
                fill: true,
                tension: 0.1
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,
            interaction: { intersect: false, mode: 'index' },
            plugins: {
                legend: { display: false }
            },
            scales: {
                x: { type: "linear", title: { display: true, text: "Frequency (Hz)", font: { size: 11 } }, min: 0, max: SAMPLE_RATE / 2, grid: { color: '#f0f0f0' } },
                y: { title: { display: true, text: "Magnitude (g)", font: { size: 11 } }, min: 0, grid: { color: '#f0f0f0' } }
            }
        }
    });
}

// ================= CONTROLS =================
function updateYAxis() {
    const minVal = parseFloat(yAxisMin.value);
    const maxVal = parseFloat(yAxisMax.value);
    if (!isNaN(minVal) && !isNaN(maxVal) && minVal < maxVal) {
        timeChart.options.scales.y.min = minVal;
        timeChart.options.scales.y.max = maxVal;
        timeChart.update();
    }
}

function zoomIn() {
    windowSeconds = Math.max(WINDOW_MIN, windowSeconds / 2);
    updateWindowDisplay();
}

function zoomOut() {
    windowSeconds = Math.min(WINDOW_MAX, windowSeconds * 2);
    updateWindowDisplay();
}

function updateWindowDisplay() {
    if (windowSeconds >= 1) {
        windowDisplay.textContent = windowSeconds.toFixed(1) + " s";
    } else {
        windowDisplay.textContent = (windowSeconds * 1000).toFixed(0) + " ms";
    }
}

// ================= CSV EXPORT =================
function saveDataToCSV() {
    const fileName = document.getElementById("file-name").value || "accel_data";
    let csv = "Time(s),DeviceTs(ms),X(g),Y(g),Z(g)\n";
    receivedData.forEach(d => csv += `${d.t.toFixed(4)},${d.ts || 0},${d.x.toFixed(4)},${d.y.toFixed(4)},${d.z.toFixed(4)}\n`);
    const blob = new Blob([csv], { type: "text/csv" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = fileName + ".csv";
    a.click();
}
