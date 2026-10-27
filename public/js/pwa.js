import { logActivity } from './utils.js';

if ('serviceWorker' in navigator) {
    window.addEventListener('load', function() {
        navigator.serviceWorker.register('/sw.js').catch(err => console.log("SW Config Not Found"));
    });
}

let deferredInstallPrompt = null;
const installBtn = document.getElementById('installBtn');

function isStandaloneMode() {
    return window.matchMedia('(display-mode: standalone)').matches || window.navigator.standalone === true;
}

function isIosDevice() {
    return /iphone|ipad|ipod/i.test(window.navigator.userAgent) && !window.MSStream;
}

window.addEventListener('beforeinstallprompt', (e) => {
    deferredInstallPrompt = e;
    console.log('[PWA] beforeinstallprompt captured — Install App button is now backed by the native prompt.');
});

window.addEventListener('appinstalled', () => {
    deferredInstallPrompt = null;
    if (installBtn) installBtn.style.display = 'none';
    logActivity('Brassica was installed to the home screen.', 'success');
});

// Attaching to window so inline HTML onclick handlers can access them
window.showIosInstallModal = function() {
    const modal = document.getElementById('iosInstallModal');
    if (modal) modal.style.display = 'flex';
}

window.closeIosInstallModal = function() {
    const modal = document.getElementById('iosInstallModal');
    if (modal) modal.style.display = 'none';
}

window.handleInstallClick = async function() {
    if (deferredInstallPrompt) {
        deferredInstallPrompt.prompt();
        const choice = await deferredInstallPrompt.userChoice;
        logActivity(`Install ${choice.outcome === 'accepted' ? 'started' : 'dismissed'} by user.`, 'info');
        deferredInstallPrompt = null;
    } else if (isIosDevice()) {
        window.showIosInstallModal();
    } else {
        alert('To install Brassica:\n\nOpen your browser menu (⋮ or the Share icon) and choose "Add to Home screen" or "Install app".\n\nIf that option is missing, this page may need to be loaded over HTTPS first.');
    }
}

if (isStandaloneMode() && installBtn) {
    installBtn.style.display = 'none';
}

if (isIosDevice() && !isStandaloneMode()) {
    window.addEventListener('load', () => setTimeout(window.showIosInstallModal, 1200));
}