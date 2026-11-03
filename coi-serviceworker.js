// coi-serviceworker.js - GitHub Pages can't send COOP/COEP headers, but WebAssembly
// threads (SharedArrayBuffer) need a cross-origin-isolated page. This service worker
// adds the two headers to same-origin responses, then reloads the page once.
if (typeof window === 'undefined') {
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
  self.addEventListener('fetch', (e) => {
    const r = e.request;
    if (r.cache === 'only-if-cached' && r.mode !== 'same-origin') return;
    if (new URL(r.url).origin !== self.location.origin) return;   // Reddit API calls go straight through
    e.respondWith(fetch(r).then((res) => {
      if (res.status === 0) return res;
      const h = new Headers(res.headers);
      h.set('Cross-Origin-Embedder-Policy', 'require-corp');
      h.set('Cross-Origin-Opener-Policy', 'same-origin');
      return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
    }));
  });
} else {
  (async () => {
    if (window.crossOriginIsolated) return;
    if (!('serviceWorker' in navigator)) return;
    const reg = await navigator.serviceWorker.register(window.document.currentScript.src);
    reg.addEventListener('updatefound', () => window.location.reload());
    if (reg.active && !navigator.serviceWorker.controller) window.location.reload();
  })();
}
