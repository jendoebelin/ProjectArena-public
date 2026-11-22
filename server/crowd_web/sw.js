// The Crowd: keeps the app's own files on the phone so it opens at once (and installs as an app). Live data always
// comes over the WebSocket, never from this cache. Bump VERSION when the files change, and index.html's ?v= with it
// (the code and style under a new name at each update: 2026-11-20, a phone's browser kept an old app.js for hours).
const VERSION = 'crowd-8';
const V = '?v=' + VERSION;
const SHELL = ['./', 'index.html', 'style.css' + V, 'app.js' + V, 'nations.js' + V, 'manifest.webmanifest', 'icon-192.png'];

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
  // (asked of the server each time, not the browser's own cache, which may hold an older copy; the page itself as it
  // comes: a navigation can't be asked that way)
  const req = e.request.mode === 'navigate' ? e.request : new Request(e.request, { cache: 'no-cache' });
  e.respondWith(fetch(req).then((resp) => {
    if (resp.ok) { const copy = resp.clone(); caches.open(VERSION).then((c) => c.put(e.request, copy)); }
    return resp;
  }).catch(() => caches.match(e.request)));
});

// notifications (Web Push): the relay sends {title, body, id} when people are playing (the bell on the page)
self.addEventListener('push', (e) => {
  let d = {};
  try { d = e.data ? e.data.json() : {}; } catch (err) { /* not JSON: the default text */ }
  e.waitUntil(self.registration.showNotification(d.title || 'People are playing Project Arena', {
    body: d.body || 'Tap to watch on The Crowd.',
    icon: 'icon-192.png',
    tag: 'live',
    renotify: true,
    data: { id: d.id || null },
    actions: [{ action: 'watch', title: 'Watch' }, { action: 'settings', title: 'Notification settings' }],
  }));
});
self.addEventListener('notificationclick', (e) => {
  e.notification.close();
  const settings = e.action === 'settings';
  const id = (e.notification.data && e.notification.data.id) || null;
  const url = './' + (settings ? '?settings=1' : id ? '?watch=' + encodeURIComponent(id) : '');
  e.waitUntil(self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then((list) => {
    for (const c of list) {   // the page is open already: bring it forward and tell it what to show
      if ('focus' in c) { c.postMessage({ t: 'open', settings, id }); return c.focus(); }
    }
    return self.clients.openWindow(url);
  }));
});
