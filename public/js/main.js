import { updateDateTime } from './utils.js';
import { updateSensorUI, updateAutomationUI } from './ui.js';
import './pwa.js';        // PWA install logic (runs on import)
import './navigation.js'; // mobile tab navigation (runs on import)
import './wifi.js';       // Wi-Fi setup modal

updateDateTime();
setInterval(updateDateTime, 60000);

// ---------------------------------------------------------------------------
// Live polling
// ---------------------------------------------------------------------------
// Polls every 5 s while you are using the page, every 30 s once it has been
// idle for 5 minutes, and not at all while the tab is hidden. (A forgotten
// background tab polling every few seconds used up the free request quotas
// of Vercel and Redis, after which the site refused requests and flickered.)
// Two more layers keep the ESP32 badge from flapping:
//   1. the server only declares "disconnected" after 35 s of silence;
//   2. the page needs OFFLINE_CONFIRM_POLLS disconnected answers in a row
//      before it switches the whole dashboard to the offline view.
const POLL_MS = 5000;
const IDLE_POLL_MS = 30000;
const IDLE_AFTER_MS = 5 * 60 * 1000;
const REQUEST_TIMEOUT_MS = 8000;
const OFFLINE_CONFIRM_POLLS = 2;
const BACKEND_FAILURE_THRESHOLD = 3;

let lastActivity = Date.now();

let consecutiveFailures = 0;
let offlinePolls = 0;
let lastConnectedData = null;
let storageWarning = null;
let polling = false;
let pollTimer = null;

function setBanner(message) {
    const banner = document.getElementById("backendBanner");
    const text = document.getElementById("backendBannerText");
    if (!banner) return;
    if (message) {
        if (text) text.textContent = message;
        banner.style.display = "flex";
    } else {
        banner.style.display = "none";
    }
}

function refreshBanner() {
    if (consecutiveFailures >= BACKEND_FAILURE_THRESHOLD) {
        setBanner("Can't reach the server — retrying...");
    } else {
        setBanner(storageWarning);   // null hides it
    }
}

function renderBadge(data) {
    const badge = document.getElementById("espBadge");
    const dot = document.getElementById("espDot");
    const text = document.getElementById("espText");
    if (!badge || !dot || !text) return;

    if (data.esp32Connected) {
        text.textContent = "ESP32 CONNECTED";
        badge.style.color = "var(--primary)";
        badge.title = data.wifiSsid ? `Wi-Fi: ${data.wifiSsid}${data.ip ? " · " + data.ip : ""}` : "";
        dot.style.background = "var(--primary)";
        dot.style.boxShadow = "0 0 10px var(--primary)";
        dot.style.animation = "pulse 2s infinite";
    } else {
        text.textContent = "ESP32 DISCONNECTED";
        badge.style.color = "var(--danger)";
        badge.title = "";
        dot.style.background = "var(--danger)";
        dot.style.boxShadow = "none";
        dot.style.animation = "none";
    }
}

function render(data) {
    updateSensorUI(data);
    updateAutomationUI(data);
    renderBadge(data);
}

function handleData(data) {
    storageWarning = data.storageWarning || null;
    if (typeof data.pumpPinRequired === "boolean") window.__pumpPinRequired = data.pumpPinRequired;

    if (data.esp32Connected) {
        offlinePolls = 0;
        lastConnectedData = data;
        render(data);
    } else {
        offlinePolls += 1;
        if (lastConnectedData && offlinePolls < OFFLINE_CONFIRM_POLLS) {
            // One "disconnected" answer can be a blip: keep the last good view.
        } else {
            lastConnectedData = null;
            render(data);
        }
    }

    const footer = document.getElementById("footerStatus");
    if (footer) {
        const time = new Date().toLocaleTimeString("en-US", { hour: "numeric", minute: "2-digit", second: "2-digit" });
        footer.textContent = `Last sync ${time}`;
    }
}

async function poll() {
    if (polling) return;
    polling = true;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
    try {
        const response = await fetch("/api/sensors", { cache: "no-store", signal: controller.signal });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.json();
        consecutiveFailures = 0;
        handleData(data);
    } catch (err) {
        consecutiveFailures += 1;
        console.warn("Could not reach /api/sensors:", err);
        const footer = document.getElementById("footerStatus");
        if (footer) footer.textContent = "Connection lost, retrying...";
    } finally {
        clearTimeout(timeout);
        polling = false;
        refreshBanner();
    }
}

function schedule() {
    clearTimeout(pollTimer);
    const idle = Date.now() - lastActivity > IDLE_AFTER_MS;
    pollTimer = setTimeout(async () => {
        if (!document.hidden) await poll();
        schedule();
    }, idle ? IDLE_POLL_MS : POLL_MS);
}

// Any interaction wakes an idle page up immediately.
let lastWake = 0;
["pointerdown", "keydown", "touchstart", "scroll", "mousemove"].forEach((evt) => {
    window.addEventListener(evt, () => {
        const now = Date.now();
        const wasIdle = now - lastActivity > IDLE_AFTER_MS;
        lastActivity = now;
        if (wasIdle && now - lastWake > 2000 && !document.hidden) {
            lastWake = now;
            clearTimeout(pollTimer);
            poll().finally(schedule);
        }
    }, { passive: true });
});

document.addEventListener("visibilitychange", () => {
    if (!document.hidden) {
        clearTimeout(pollTimer);
        poll().finally(schedule);   // refresh immediately when you come back to the tab
    }
});

poll().finally(schedule);

// ---------------------------------------------------------------------------
// 7-day averages
// ---------------------------------------------------------------------------
async function loadHistoricalData() {
    try {
        const response = await fetch("/api/history", { cache: "no-store" });
        if (!response.ok) return;
        const data = await response.json();

        if (data.status === "success" && data.history.length > 0) {
            const recentList = document.querySelector(".panel-strip .history-list");
            if (recentList) {
                recentList.innerHTML = "";
                data.history.slice(-1).forEach(day => {
                    recentList.innerHTML += `
                        <div class="history-item">
                            <span class="history-date">${day.date}</span>
                            <div class="history-stats">
                                <span class="t"><i class="ph-fill ph-thermometer"></i> ${day.temp}&deg;C</span>
                                <span class="h"><i class="ph-fill ph-drop"></i> ${day.hum}%</span>
                                <span class="s" title="Soil 1"><i class="ph-fill ph-potted-plant"></i> S1: ${day.soil1}%</span>
                                <span class="s" title="Soil 2"><i class="ph-fill ph-potted-plant"></i> S2: ${day.soil2}%</span>
                            </div>
                        </div>`;
                });
            }

            const modalList = document.getElementById("modalHistoryList");
            if (modalList) {
                modalList.innerHTML = "";
                data.history.slice().reverse().forEach(day => {
                    modalList.innerHTML += `
                        <div style="display: grid; grid-template-columns: 1.2fr 1fr 1fr 1fr 1fr; align-items: center; padding: 10px 12px; background: linear-gradient(145deg, rgba(255,255,255,0.06), rgba(255,255,255,0.01)); border: 1px solid rgba(255,255,255,0.04); border-radius: 10px;">
                            <div style="display: flex; flex-direction: column; gap: 2px;">
                                <span style="font-size: 11px; font-weight: 600; color: var(--text-main);">${day.date}</span>
                            </div>
                            <div style="text-align: center; color: var(--c-temp); font-size: 11px; font-weight: 700;">${day.temp}&deg;</div>
                            <div style="text-align: center; color: var(--c-hum); font-size: 11px; font-weight: 700;">${day.hum}%</div>
                            <div style="text-align: center; color: var(--c-soil); font-size: 11px; font-weight: 700;">${day.soil1}%</div>
                            <div style="text-align: right; color: var(--c-soil); font-size: 11px; font-weight: 700;">${day.soil2}%</div>
                        </div>`;
                });
            }
        }
    } catch (err) {
        console.warn("Could not load historical averages:", err);
    }
}

loadHistoricalData();
setInterval(loadHistoricalData, 5 * 60 * 1000);


// ---------------------------------------------------------------------------
// Automation Activity Log
// ---------------------------------------------------------------------------

let availableLogDates = [];
let currentLogDateIndex = 0;

async function loadAutomationLog(targetDate = "") {
    try {
        const url = targetDate ? `/api/activity?date=${targetDate}` : "/api/activity";
        const response = await fetch(url, { cache: "no-store" });
        if (!response.ok) return;
        const data = await response.json();
        
        const container = document.getElementById("automationLogList");
        const dateLabel = document.getElementById("activityDateLabel");
        if (!container || !dateLabel) return;

        if (data.dates && data.dates.length > 0) {
            availableLogDates = data.dates;
            currentLogDateIndex = availableLogDates.indexOf(data.current_date);
            
            // Format display date (e.g., "2026-10-22" -> "Oct 22, 2026")
            const parsedDate = new Date(data.current_date + "T00:00:00");
            dateLabel.textContent = parsedDate.toLocaleDateString("en-US", { month: "short", day: "numeric", year: "numeric" });
        } else {
            dateLabel.textContent = "No Data";
        }

        if (!data.events || data.events.length === 0) {
            container.innerHTML = '<span style="font-size: 12px; color: var(--text-muted); text-align: center; padding: 12px 0;">No activation events recorded for this date.</span>';
            return;
        }

        container.innerHTML = data.events.map(ev => {
            const isPump = ev.type === "pump";
            const icon = isPump ? '<i class="ph-fill ph-drop"></i>' : '<i class="ph-fill ph-fan"></i>';
            const color = isPump ? 'var(--primary)' : 'var(--text-muted)';
            
            return `
            <div style="display: flex; align-items: center; justify-content: space-between; background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.04); padding: 10px 14px; border-radius: 12px;">
                <div style="display: flex; align-items: center; gap: 10px;">
                    <div style="display: flex; align-items: center; justify-content: center; width: 32px; height: 32px; background: rgba(255,255,255,0.05); border-radius: 50%; color: ${color}; font-size: 15px;">
                        ${icon}
                    </div>
                    <div style="display: flex; flex-direction: column; gap: 2px;">
                        <span style="font-size: 12px; font-weight: 600; color: var(--text-main);">${ev.msg}</span>
                        <span style="font-size: 10px; color: var(--text-muted);">${ev.time}</span>
                    </div>
                </div>
                <span style="font-size: 10px; font-weight: 600; color: var(--primary); background: var(--primary-soft); padding: 2px 8px; border-radius: 50px;">Triggered</span>
            </div>`;
        }).join("");
    } catch (err) {
        console.warn("Could not load activity log:", err);
    }
}

window.changeActivityDate = function(direction) {
    if (!availableLogDates.length) return;
    // Moving left/right through sorted dates (newer <-> older)
    currentLogDateIndex -= direction; 
    if (currentLogDateIndex < 0) currentLogDateIndex = 0;
    if (currentLogDateIndex >= availableLogDates.length) currentLogDateIndex = availableLogDates.length - 1;
    
    loadAutomationLog(availableLogDates[currentLogDateIndex]);
};

loadAutomationLog();// Refresh timeline every 5 minutes
// ---------------------------------------------------------------------------
// History modal (Wi-Fi modal lives in wifi.js)
// ---------------------------------------------------------------------------
window.openHistoryModal = function () {
    document.getElementById("historyModal").style.display = "flex";
};
window.closeHistoryModal = function () {
    document.getElementById("historyModal").style.display = "none";
};

// ---------------------------------------------------------------------------
// Manual Overrides
// ---------------------------------------------------------------------------
const pumpEl = () => document.getElementById("manualPumpBtn");
const toast = (msg, kind) => { if (window.logActivity) window.logActivity(msg, kind); };

// Sends the pump command. ON needs the PIN (checked by the server); OFF never does.
// On success the switch moves to the new state; on any failure it stays where it really is.
async function applyPump(newState, pin) {
    const toggle = pumpEl();
    toggle.disabled = true;
    try {
        const res = await fetch("/api/control", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ target: "pump", state: newState, pin })
        });
        let body = null;
        try { body = await res.json(); } catch (_) { /* non-JSON error page */ }

        if (res.ok) {
            toggle.checked = newState;
            toast(`Pump manually turned ${newState ? "ON" : "OFF"}`, "success");
            return { ok: true };
        }
        toggle.checked = !newState;
        const message = (body && body.message) || "Server rejected command";
        if (res.status !== 403 && res.status !== 429) toast("Failed to send pump command", "danger");
        return { ok: false, status: res.status, message };
    } catch (err) {
        console.warn("Manual override failed:", err);
        toggle.checked = !newState;
        toast("Failed to send pump command", "danger");
        return { ok: false, status: 0, message: "Could not reach the server." };
    } finally {
        toggle.disabled = false;
    }
}

window.togglePump = async function () {
    const toggle = pumpEl();
    const wantOn = toggle.checked;

    if (!wantOn) {                       // turning OFF: no PIN, apply immediately
        await applyPump(false, "");
        return;
    }
    toggle.checked = false;              // the switch only moves once the PIN is accepted
    if (window.__pumpPinRequired === false) {
        await applyPump(true, "");       // server says no PIN is configured
    } else {
        window.openPumpPinModal();
    }
};

window.openPumpPinModal = function () {
    const input = document.getElementById("pumpPinInput");
    document.getElementById("pumpPinError").textContent = "";
    input.value = "";
    document.getElementById("pumpPinModal").style.display = "flex";
    input.focus();
};

window.closePumpPinModal = function () {
    document.getElementById("pumpPinInput").value = "";
    document.getElementById("pumpPinModal").style.display = "none";
};

window.submitPumpPin = async function () {
    const input = document.getElementById("pumpPinInput");
    const errorEl = document.getElementById("pumpPinError");
    const btn = document.getElementById("pumpPinBtn");
    const pin = input.value;

    errorEl.style.color = "var(--danger)";
    if (!pin) { errorEl.textContent = "Enter the PIN."; return; }

    btn.disabled = true;
    errorEl.style.color = "var(--text-muted)";
    errorEl.textContent = "Checking...";
    const result = await applyPump(true, pin);
    btn.disabled = false;

    if (result.ok) {
        window.closePumpPinModal();
    } else {
        errorEl.style.color = "var(--danger)";
        errorEl.textContent = result.message;
        input.value = "";
        input.focus();
    }
};