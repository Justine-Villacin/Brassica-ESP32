export function updateDateTime() {
    const dateElement = document.getElementById("currentDate");
    const now = new Date();
    const options = { weekday: "long", year: "numeric", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" };
    if (dateElement) dateElement.textContent = now.toLocaleDateString("en-US", options);
}

// Shows a short message. If the page has an #activityFeed it is added there;
// otherwise (the current layout) it appears as a small toast at the bottom.
export function logActivity(message, type = "success") {
    let color = "var(--primary)";
    if (type === "danger") color = "var(--danger)";
    if (type === "warning") color = "var(--warning)";
    if (type === "info") color = "var(--info, #60a5fa)";

    const container = document.getElementById("activityFeed");
    if (!container) {
        showToast(message, color);
        return;
    }

    const time = new Date().toLocaleTimeString("en-US", { hour: "numeric", minute: "2-digit" });
    const row = document.createElement("div");
    row.className = "activity-row";

    const dot = document.createElement("div");
    dot.className = "activity-dot";
    dot.style.background = color;
    dot.style.boxShadow = `0 0 8px ${color}`;

    const text = document.createElement("span");
    text.textContent = message;

    const timeEl = document.createElement("span");
    timeEl.className = "activity-time";
    timeEl.textContent = time;

    row.append(dot, text, timeEl);
    container.prepend(row);
    while (container.children.length > 5) {
        container.removeChild(container.lastElementChild);
    }
}

function showToast(message, color) {
    let host = document.getElementById("toastHost");
    if (!host) {
        host = document.createElement("div");
        host.id = "toastHost";
        host.className = "toast-host";
        document.body.appendChild(host);
    }
    const toast = document.createElement("div");
    toast.className = "toast";
    toast.style.borderColor = color;
    toast.textContent = message;
    host.appendChild(toast);
    setTimeout(() => toast.remove(), 5000);
    while (host.children.length > 3) host.removeChild(host.firstElementChild);
}

window.logActivity = logActivity; // Expose to global scope for inline HTML onclick handlers

// Fetch-driven download used by the CSV/Excel export buttons. Replaces the
// old plain <a href> links, which navigated the whole tab to a bare 404
// text page whenever history wasn't available. This instead reads the
// response, and:
//   - on success: turns it into a real client-side file download
//   - on failure: shows the server's message as a toast in the activity
//     feed, so the failure looks and feels like the rest of the app
//     instead of a jarring unstyled error page.
export async function downloadFile(url, filename) {
    try {
        const response = await fetch(url);
        const contentType = response.headers.get("Content-Type") || "";

        if (!response.ok || contentType.includes("application/json")) {
            let message = "Export failed. Please try again.";
            try {
                const body = await response.json();
                if (body && body.message) message = body.message;
            } catch (_) {
                // Response wasn't JSON either; keep the default message.
            }
            logActivity(message, "warning");
            return;
        }

        const blob = await response.blob();
        const link = document.createElement("a");
        link.href = URL.createObjectURL(blob);
        link.download = filename;
        document.body.appendChild(link);
        link.click();
        link.remove();
        URL.revokeObjectURL(link.href);
        logActivity(`Downloaded ${filename}`, "success");
    } catch (err) {
        console.error("Download error:", err);
        logActivity("Could not reach the server for this export.", "danger");
    }
}

window.downloadFile = downloadFile; // Expose to global scope for inline HTML onclick handlers