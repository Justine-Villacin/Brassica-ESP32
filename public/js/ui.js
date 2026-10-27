export function updateAutomationUI(data) {
    // --- Manual Pump Button Sync ---
    const manualPumpBtn = document.getElementById("manualPumpBtn");
    if (manualPumpBtn) {
        if (data.pump) {
            manualPumpBtn.setAttribute("data-state", "on");
            manualPumpBtn.textContent = "Turn OFF";
            manualPumpBtn.style.background = "var(--primary)";
            manualPumpBtn.style.color = "#000";
        } else {
            manualPumpBtn.setAttribute("data-state", "off");
            manualPumpBtn.textContent = "Turn ON";
            manualPumpBtn.style.background = "transparent";
            manualPumpBtn.style.color = "#fff";
        }
    }

    // --- Auto-Watering Card ---
    const pumpCard = document.getElementById("autoWateringCard");
    const pumpPill = document.getElementById("pumpPill");
    if (pumpCard) pumpCard.classList.remove("active-pump", "offline");
    
    if (!data.soilConnected) {
        if (pumpCard) pumpCard.classList.add("offline");
        document.getElementById("pumpTitle").textContent = "Auto-Watering";
        document.getElementById("pumpDesc").textContent = "Soil sensor disconnected — disabled.";
        if (pumpPill) pumpPill.textContent = "Offline";
    } else if (data.pump && !data.manualPumpActive) {
        if (pumpCard) pumpCard.classList.add("active-pump");
        document.getElementById("pumpTitle").textContent = "Auto-Watering";
        document.getElementById("pumpDesc").textContent = "Soil moisture is low. Pumping water.";
        if (pumpPill) pumpPill.textContent = "Active";
    } else {
        document.getElementById("pumpTitle").textContent = "Auto-Watering";
        document.getElementById("pumpDesc").textContent = "Soil is hydrated.";
        if (pumpPill) pumpPill.textContent = "Standby";
    }
    
    // --- Exhaust Fan Card ---
    const fanCard = document.getElementById("autoFanCard");
    const fanPill = document.getElementById("fanPill");
    if (fanCard) fanCard.classList.remove("active-fan", "offline");
    
    if (!data.dhtConnected) {
        if (fanCard) fanCard.classList.add("offline");
        document.getElementById("fanTitle").textContent = "Exhaust Fan";
        document.getElementById("fanDesc").textContent = "Temperature sensor disconnected — disabled.";
        if (fanPill) fanPill.textContent = "Offline";
    } else if (data.fan) {
        if (fanCard) fanCard.classList.add("active-fan");
        document.getElementById("fanTitle").textContent = "Exhaust Fan";
        document.getElementById("fanDesc").textContent = "High temp detected. Cooling down.";
        if (fanPill) fanPill.textContent = "Active";
    } else {
        document.getElementById("fanTitle").textContent = "Exhaust Fan";
        document.getElementById("fanDesc").textContent = "Temperature is optimal.";
        if (fanPill) fanPill.textContent = "Standby";
    }
}

function setSensorOffline(cardSelector, isOffline) {
    const card = document.querySelector(cardSelector);
    if (card) card.classList.toggle("offline", isOffline);
}

export function updateSensorUI(data) {
    const tempEl = document.getElementById("temperature");
    const humEl  = document.getElementById("humidity");
    const soilEl = document.getElementById("soil");
    const soil2El = document.getElementById("soil2"); 

    const tempStatus  = tempEl  && tempEl.closest(".reading").querySelector(".reading-state");
    const humStatus   = humEl   && humEl.closest(".reading").querySelector(".reading-state");
    const soilStatus  = soilEl  && soilEl.closest(".reading").querySelector(".reading-state");
    const soil2Status = soil2El && soil2El.closest(".reading").querySelector(".reading-state");

    setSensorOffline(".sensor.temperature", !data.dhtConnected);
    setSensorOffline(".sensor.humidity",    !data.dhtConnected);

    if (tempEl && humEl) {
        if (!data.dhtConnected) {
            tempEl.textContent = "--";
            if (tempStatus) { tempStatus.textContent = "OFFLINE"; tempStatus.style.color = "var(--danger)"; }
            humEl.textContent = "--";
            if (humStatus) { humStatus.textContent = "OFFLINE"; humStatus.style.color = "var(--danger)"; }
        } else {
            tempEl.textContent = Number(data.temp).toFixed(1);
            if (tempStatus) {
                tempStatus.textContent = data.temp >= 30 ? "WARNING" : "NORMAL";
                tempStatus.style.color = data.temp >= 30 ? "var(--danger)" : "var(--primary)";
            }
            humEl.textContent = Math.round(data.hum);
            if (humStatus) {
                humStatus.textContent = data.hum >= 60 && data.hum <= 80 ? "NORMAL" : "WARNING";
                humStatus.style.color = data.hum >= 60 && data.hum <= 80 ? "var(--primary)" : "var(--warning)";
            }
        }
    }

    // ----- Soil 1 -----
    const soil1Ok = !!data.soil1Connected && data.soil !== null && data.soil !== undefined;
    if (soilEl) {
        if (!soil1Ok) {
            soilEl.textContent = "--";
            if (soilStatus) {
                soilStatus.textContent = "OFFLINE";
                soilStatus.style.color = "var(--danger)";
            }
        } else {
            soilEl.textContent = Math.round(data.soil);
            if (soilStatus) {
                soilStatus.textContent = data.soil <= 40 ? "LOW" : "GOOD";
                soilStatus.style.color = data.soil <= 40 ? "var(--danger)" : "var(--primary)";
            }
        }
    }

    // ----- Soil 2 -----
    const soil2Ok = !!data.soil2Connected && data.soil2 !== null && data.soil2 !== undefined;
    if (soil2El) {
        if (!soil2Ok) {
            soil2El.textContent = "--";
            if (soil2Status) {
                soil2Status.textContent = "OFFLINE";
                soil2Status.style.color = "var(--danger)";
            }
        } else {
            soil2El.textContent = Math.round(data.soil2);
            if (soil2Status) {
                soil2Status.textContent = data.soil2 <= 40 ? "LOW" : "GOOD";
                soil2Status.style.color = data.soil2 <= 40 ? "var(--danger)" : "var(--primary)";
            }
        }
    }

    let score = 100;

    if (data.dhtConnected) {
        if (data.temp >= 30) score -= 25;
        else if (data.temp < 15) score -= 15;
        if (data.hum > 85) score -= 15;
        else if (data.hum < 50) score -= 20;
    } else {
        score -= 45;
    }

    if (data.soilConnected) {
        if (data.soil <= 40) score -= 35;
        else if (data.soil > 85) score -= 10;
    } else {
        score -= 35;
    }

    score = Math.max(0, score);
    const healthScoreEl = document.getElementById("healthScore");
    if (healthScoreEl) {
        healthScoreEl.textContent = `${score}%`;
        const ringContainer = document.getElementById("vigorRingContainer");
        if (ringContainer) {
            ringContainer.style.setProperty('--pct', score);
        }
    }

    // --- Top Climate Text Updater ---
    const climateText = document.querySelector(".climate-text");
    if (climateText) {
        let activeActs = [];
        if (data.fan) activeActs.push("Fan Cooling");
        if (data.pump) activeActs.push("Watering");

        if (activeActs.length > 0) {
            climateText.innerHTML = `<strong>Climate System Active</strong> Regulating: ${activeActs.join(", ")}.`;
        } else {
            climateText.innerHTML = `<strong>Climate System Stable</strong> All metrics within optimal thresholds.`;
        }
    }
}