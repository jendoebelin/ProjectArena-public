// The Crowd: keeps the app's own files on the phone so it opens at once (and installs as an app). Live data always
// comes over the WebSocket, never from this cache. Bump VERSION when the files change.
const VERSION = 'crowd-4';
const SHELL = ['./', 'index.html', 'style.css', 'app.js', 'nations.js', 'manifest.webmanifest', 'icon-192.png'];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(VERSION).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys().then((keys) => Promise.all(keys.filter((k) => k !== VERSION).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});
self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== 'GET' || url.origin !== location.origin || url.pathname.endsWith('/ws')) return;
  // the page and its code: the network first (updates show at once), the cache when offline; flags: the cache first
  if (url.pathname.includes('/flags/')) {
    e.respondWith(caches.match(e.request).then((r) => r || fetch(e.request).then((resp) => {
      if (resp.ok) { const copy = resp.clone(); caches.open(VERSION).then((c) => c.put(e.request, copy)); }
      return resp;
    })));
    return;
  }
  e.respondWith(fetch(e.request).then((resp) => {
    if (resp.ok) { const copy = resp.clone(); caches.open(VERSION).then((c) => c.put(e.request, copy)); }
    return resp;
  }).catch(() => caches.match(e.request)));
});
