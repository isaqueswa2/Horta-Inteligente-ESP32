// Service worker do app da Horta (PWA no GitHub Pages).
// - Página: rede primeiro (sempre a versão mais nova); sem internet, a cópia guardada.
// - SDK do Firebase (URL com versão, nunca muda): cache primeiro, o app abre mais rápido.
// - Firebase (banco e login) passa direto: os dados são sempre ao vivo.
const CACHE = 'horta-v1';
const BASICOS = ['./', './manifest.webmanifest', './icone-192.png', './icone-512.png'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(BASICOS)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(caches.keys()
    .then(nomes => Promise.all(nomes.filter(n => n !== CACHE).map(n => caches.delete(n))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);

  if (url.origin === 'https://www.gstatic.com' && url.pathname.startsWith('/firebasejs/')) {
    e.respondWith(caches.match(req).then(r => r || fetch(req).then(resp => {
      const copia = resp.clone();
      caches.open(CACHE).then(c => c.put(req, copia));
      return resp;
    })));
    return;
  }

  if (url.origin !== location.origin) return;
  e.respondWith(fetch(req).then(resp => {
    if (resp.ok) {
      const copia = resp.clone();
      caches.open(CACHE).then(c => c.put(req.mode === 'navigate' ? './' : req, copia));
    }
    return resp;
  }).catch(() => caches.match(req.mode === 'navigate' ? './' : req)));
});
