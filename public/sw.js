// Brassica Service Worker
//
// A registered, working service worker + a valid manifest.json served over
// HTTPS are what Chrome/Edge/Samsung Internet require before they treat this
// site as an installable PWA.

const CACHE_NAME = 'brassica-v1';

// Files that make up the app "shell", cached so the dashboard still opens
// with a flaky Wi-Fi link.
const APP_SHELL = [
    '/',
    '/css/main.css',
    '/js/main.js',
    '/manifest.json',
    '/icons/icon-192.png',
    '/icons/icon-512.png'
];

self.addEventListener('install', (event) => {
    event.waitUntil(
        caches.open(CACHE_NAME).then((cache) =>
            // cache.add() one-by-one (not addAll) so ONE missing file can't
            // abort the whole install.
            Promise.all(APP_SHELL.map((url) =>
                cache.add(url).catch((err) => console.warn('[Service Worker] Could not cache', url, err))
            ))
        ).then(() => self.skipWaiting())
    );
});

self.addEventListener('activate', (event) => {
    event.waitUntil(
        caches.keys()
            .then((keys) => Promise.all(
                keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key))
            ))
            .then(() => clients.claim())
    );
});

self.addEventListener('fetch', (event) => {
    const request = event.request;

    // Only plain GETs are ever cached. POSTs (the ESP32 reporting, Wi-Fi
    // changes) go straight to the network untouched.
    if (request.method !== 'GET') return;

    const url = new URL(request.url);

    // Other sites (fonts, icon CDN) are left to the browser.
    if (url.origin !== self.location.origin) return;

    // Dev-only live-reload stream: never intercept (needed for SSE).
    if (url.pathname === '/dev/reload-stream') return;

    // Live data and exports must always hit the network: never serve stale
    // sensor readings from cache, and never throw when offline.
    if (url.pathname.startsWith('/api/') || url.pathname === '/update'
        || url.pathname === '/sensor_data.csv' || url.pathname === '/download/excel') {
        event.respondWith(
            fetch(request).catch(() =>
                new Response(JSON.stringify({ status: 'error', message: 'offline' }), {
                    status: 503,
                    headers: { 'Content-Type': 'application/json' }
                })
            )
        );
        return;
    }

    // App shell / static assets: network-first, falling back to cache offline.
    event.respondWith(
        fetch(request)
            .then((response) => {
                if (response && response.ok) {
                    const copy = response.clone();
                    caches.open(CACHE_NAME).then((cache) => cache.put(request, copy)).catch(() => {});
                }
                return response;
            })
            .catch(() => caches.match(request).then((cached) =>
                cached || new Response('Offline', { status: 503, headers: { 'Content-Type': 'text/plain' } })
            ))
    );
});
