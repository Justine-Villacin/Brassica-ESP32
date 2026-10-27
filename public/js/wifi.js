// Wi-Fi setup modal.
//
// How it works: the ESP32 scans for nearby networks and reports them to the
// server. This modal lists them; when you pick one and enter the password, the
// server hands the request to the ESP32 in its next reply. The ESP32 tries the
// network, falls back to the old one if it fails, and reports the result,
// which this modal then shows. Nothing needs re-flashing.
//
// Every network name is placed with textContent (never innerHTML), so a
// neighbour's strangely named Wi-Fi can't inject anything into the page.

const $ = (id) => document.getElementById(id);

const SCAN_POLL_MS = 2500;
const RESULT_POLL_MS = 2000;
const RESULT_TIMEOUT_MS = 120000;

let pinRequired = false;
let currentSsid = "";
let manualEntry = false;
let scanTimer = null;
let resultTimer = null;
let lastNetworks = [];

function clearTimers() {
    clearTimeout(scanTimer);
    clearTimeout(resultTimer);
    scanTimer = null;
    resultTimer = null;
}

async function api(path, options) {
    const response = await fetch(path, { cache: "no-store", ...options });
    let body = null;
    try { body = await response.json(); } catch (_) { /* non-JSON error page */ }
    return { ok: response.ok, status: response.status, body };
}

function setView(view) {
    // view: "list" | "form" | "progress"
    $("wifiListSection").style.display = view === "list" ? "block" : "none";
    $("wifiPasswordSection").style.display = view === "form" ? "flex" : "none";
    $("wifiProgressSection").style.display = view === "progress" ? "flex" : "none";
}

function message(el, text, kind) {
    el.textContent = text;
    el.style.color = kind === "error" ? "var(--danger)"
        : kind === "ok" ? "var(--primary)"
        : kind === "warn" ? "var(--warning)"
        : "var(--text-muted)";
}

// ---- device status line ----------------------------------------------------
async function loadStatus() {
    try {
        const { ok, body } = await api("/api/wifi/status");
        if (!ok || !body) throw new Error("status failed");

        pinRequired = !!body.pin_required;
        $("wifiPinWrap").style.display = pinRequired ? "block" : "none";
        $("wifiOpenWarning").style.display = body.open_access_warning ? "block" : "none";

        const dev = body.device || {};
        currentSsid = dev.connected ? (dev.ssid || "") : "";
        if (dev.connected) {
            const bits = [];
            if (dev.ip) bits.push(dev.ip);
            if (dev.rssi) bits.push(`${dev.rssi} dBm`);
            message($("wifiDeviceLine"),
                `ESP32 is online${dev.ssid ? " on " + dev.ssid : ""}${bits.length ? " (" + bits.join(", ") + ")" : ""}`, "ok");
        } else {
            message($("wifiDeviceLine"),
                "ESP32 is offline. You can still queue a network; it applies the next time the ESP32 checks in.", "warn");
        }
        if (lastNetworks.length) renderNetworks(lastNetworks);
    } catch (err) {
        message($("wifiDeviceLine"), "Could not read the ESP32 status.", "error");
    }
}

// ---- network list ----------------------------------------------------------
function signalIcon(rssi) {
    if (rssi < -85) return { icon: "ph-wifi-low", color: "var(--danger)" };
    if (rssi < -70) return { icon: "ph-wifi-medium", color: "var(--warning)" };
    return { icon: "ph-wifi-high", color: "var(--primary)" };
}

function renderNetworks(networks) {
    const container = $("wifiListContainer");
    container.replaceChildren();

    if (!networks.length) {
        const empty = document.createElement("div");
        empty.className = "wifi-empty";
        empty.textContent = "No networks reported yet. Tap Rescan; the ESP32 refreshes the list within about 30 seconds.";
        container.append(empty);
        return;
    }

    networks.forEach((net) => {
        const row = document.createElement("button");
        row.type = "button";
        row.className = "wifi-net";

        const name = document.createElement("span");
        name.className = "wifi-net-name";
        name.textContent = net.ssid;
        if (net.ssid === currentSsid) {
            const tag = document.createElement("small");
            tag.textContent = "Connected";
            name.append(tag);
        }

        const right = document.createElement("span");
        right.className = "wifi-net-meta";
        if (net.secure) {
            const lock = document.createElement("i");
            lock.className = "ph-bold ph-lock-key";
            right.append(lock);
        }
        const sig = signalIcon(net.rssi);
        const bars = document.createElement("i");
        bars.className = `ph-bold ${sig.icon}`;
        bars.style.color = sig.color;
        right.append(bars);

        row.append(name, right);
        row.addEventListener("click", () => selectNetwork(net.ssid, net.secure, false));
        container.append(row);
    });
}

async function loadScan(triesLeft) {
    clearTimeout(scanTimer);
    try {
        const { ok, body } = await api("/api/wifi/scan");
        if (ok && body) {
            lastNetworks = body.networks || [];
            renderNetworks(lastNetworks);
            const waiting = body.scanning || !lastNetworks.length;
            if (waiting && triesLeft > 0 && $("wifiModal").style.display !== "none") {
                scanTimer = setTimeout(() => loadScan(triesLeft - 1), SCAN_POLL_MS);
            }
            return;
        }
        throw new Error("scan failed");
    } catch (err) {
        $("wifiListContainer").replaceChildren();
        const failed = document.createElement("div");
        failed.className = "wifi-empty";
        failed.style.color = "var(--danger)";
        failed.textContent = "Could not load the network list.";
        $("wifiListContainer").append(failed);
    }
}

async function rescan() {
    const btn = $("wifiRescanBtn");
    btn.disabled = true;
    try {
        await api("/api/wifi/rescan", { method: "POST" });
    } catch (_) { /* the list below still loads */ }
    btn.disabled = false;
    loadScan(20);
}

// ---- password form ---------------------------------------------------------
function selectNetwork(ssid, secure, manual) {
    manualEntry = manual;
    const ssidInput = $("modalWifiSsid");
    ssidInput.value = ssid;
    ssidInput.readOnly = !manual;
    $("modalWifiPass").value = "";
    $("modalWifiPass").type = "password";
    $("wifiShowPass").checked = false;
    $("wifiPassWrap").style.display = secure || manual ? "block" : "none";
    message($("wifiFormError"), "", "");
    setView("form");
    (manual ? ssidInput : $("modalWifiPass")).focus();
}

function cancelNetworkSelect() {
    setView("list");
}

async function submitNetwork() {
    const ssid = $("modalWifiSsid").value;
    const password = $("wifiPassWrap").style.display === "none" ? "" : $("modalWifiPass").value;
    const pin = $("modalWifiPin").value;
    const errorEl = $("wifiFormError");

    if (!ssid) return message(errorEl, "Enter the network name.", "error");
    if (password && (password.length < 8 || password.length > 63)) {
        return message(errorEl, "Wi-Fi passwords are 8-63 characters.", "error");
    }
    if (pinRequired && !pin) return message(errorEl, "Enter the dashboard PIN.", "error");

    const btn = $("wifiConnectBtn");
    btn.disabled = true;
    message(errorEl, "Sending...", "");
    try {
        const { ok, body } = await api("/api/wifi/configure", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ ssid, password, pin }),
        });
        if (!ok || !body || body.status !== "success") {
            message(errorEl, (body && body.message) || `Could not send (HTTP ${ok ? "?" : "error"}).`, "error");
            return;
        }
        $("modalWifiPass").value = "";
        $("modalWifiPin").value = "";
        watchResult(ssid, body.id, body.message);
    } catch (err) {
        message(errorEl, "Could not reach the server.", "error");
    } finally {
        btn.disabled = false;
    }
}

// ---- progress / result -----------------------------------------------------
function watchResult(ssid, id, firstMessage) {
    setView("progress");
    const started = Date.now();
    const icon = $("wifiProgressIcon");
    const text = $("wifiProgressText");
    const done = $("wifiProgressDone");
    done.style.display = "none";
    icon.className = "ph-bold ph-spinner ph-spin";
    icon.style.color = "var(--primary)";
    message(text, firstMessage || "Sent. Waiting for the ESP32...", "");

    const step = async () => {
        clearTimeout(resultTimer);
        try {
            const { ok, body } = await api("/api/wifi/status");
            if (ok && body) {
                const result = body.result;
                if (result && result.id === id) {
                    if (result.status === "ok") {
                        icon.className = "ph-fill ph-check-circle";
                        icon.style.color = "var(--primary)";
                        message(text, `The ESP32 joined "${ssid}" and saved it. No re-flashing needed.`, "ok");
                    } else {
                        icon.className = "ph-fill ph-warning-circle";
                        icon.style.color = "var(--danger)";
                        const why = result.reason ? ` (${result.reason})` : "";
                        message(text, `The ESP32 could not join "${ssid}"${why}. It went back to its previous network.`, "error");
                    }
                    done.style.display = "block";
                    return;
                }
                if (body.pending && body.pending.id === id) {
                    message(text, body.device && body.device.connected
                        ? "Waiting for the ESP32 to pick this up (it checks for new settings about every 30 seconds)..."
                        : "The ESP32 is offline. It will apply this when it checks in.", "");
                } else {
                    message(text, "The ESP32 is switching networks. This can take up to a minute...", "");
                }
            }
        } catch (_) { /* keep trying */ }

        if (Date.now() - started > RESULT_TIMEOUT_MS) {
            icon.className = "ph-fill ph-warning-circle";
            icon.style.color = "var(--warning)";
            message(text, "No answer yet. The ESP32 may be offline or on the new network already. Check the status badge; if it stays disconnected, hold the BOOT button for 3 seconds and use the Brassica-Setup Wi-Fi.", "warn");
            done.style.display = "block";
            return;
        }
        resultTimer = setTimeout(step, RESULT_POLL_MS);
    };
    resultTimer = setTimeout(step, RESULT_POLL_MS);
}

// ---- public hooks used by the inline onclick handlers in index.html ----------
window.openWifiModal = function () {
    clearTimers();
    $("wifiModal").style.display = "flex";
    lastNetworks = [];
    setView("list");
    message($("wifiDeviceLine"), "Checking the ESP32...", "");
    $("wifiListContainer").replaceChildren();
    const loading = document.createElement("div");
    loading.className = "wifi-empty";
    loading.textContent = "Scanning networks...";
    $("wifiListContainer").append(loading);
    loadStatus();
    rescan();
};

window.closeWifiModal = function () {
    clearTimers();
    $("wifiModal").style.display = "none";
};

window.cancelNetworkSelect = cancelNetworkSelect;
window.submitNetwork = submitNetwork;
window.rescanWifi = rescan;
window.manualNetwork = () => selectNetwork("", true, true);
window.toggleWifiPassword = () => {
    $("modalWifiPass").type = $("wifiShowPass").checked ? "text" : "password";
};
