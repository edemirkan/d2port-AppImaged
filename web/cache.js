/* La caché de los archivos del juego en el navegador (va adentro de d2port.js: --pre-js; docs/web.md).
   El juego lee cada archivo con fs_read (src/fmt/fs.c): si ya está acá lo toma al instante; si no, lo pide al
   servidor y espera (ASYNCIFY). Para no pedirlos de a uno (cada pedido son milisegundos y al arrancar son ~1.500):
   - la página baja en paralelo, antes de arrancar, los de precarga.txt (lo que se lee hasta el menú) y después,
     en segundo plano, los de precarga-juego.txt (lo de entrar a jugar);
   - fs_prefetch_dir (las baldosas del acto) los baja en segundo plano.
   Lo que el juego toma sale de la caché (queda en su memoria); lo bajado de más se descarta pasado el tope. */
Module.d2cache = new Map();   /* ruta ("data/global/...") -> Uint8Array */
Module.d2pending = new Map(); /* ruta -> Promise<Uint8Array|null> (pedidos en curso) */
Module.d2bytes = 0;
Module.d2maxBytes = 192 * 1024 * 1024; /* (lo precargado que todavía no se usó, como mucho) */

/* El archivo, de la caché o del servidor (null si no está) */
Module.d2get = function (key) {
  var have = Module.d2cache.get(key);
  if (have) return Promise.resolve(have);
  var p = Module.d2pending.get(key);
  if (p) return p;
  p = fetch(key).then(function (r) { return r.ok ? r.arrayBuffer() : null; }).then(function (b) {
    Module.d2pending.delete(key);
    if (!b) return null;
    var u = new Uint8Array(b);
    if (!Module.d2cache.has(key)) {
      Module.d2cache.set(key, u);
      Module.d2bytes += u.length;
    }
    return u;
  }, function () { Module.d2pending.delete(key); return null; });
  Module.d2pending.set(key, p);
  return p;
};

/* Lo saca de la caché (lo toma el juego): Uint8Array o null */
Module.d2take = function (key) {
  var u = Module.d2cache.get(key);
  if (u) {
    Module.d2cache.delete(key);
    Module.d2bytes -= u.length;
  }
  return u || null;
};

/* Baja en paralelo (de a `parallel`) los de la lista, sin pasarse del tope; onProgress(hechos, total) */
Module.d2prefetch = function (keys, parallel, onProgress) {
  var i = 0, done = 0, total = keys.length;
  return new Promise(function (resolve) {
    if (!total) { resolve(); return; }
    function next() {
      if (i >= total) return;
      var key = keys[i++];
      var go = Module.d2cache.has(key) || Module.d2bytes > Module.d2maxBytes ? Promise.resolve() : Module.d2get(key);
      go.then(function () {
        done++;
        if (onProgress) onProgress(done, total);
        if (done === total) resolve(); else next();
      });
    }
    for (var k = 0; k < Math.min(parallel, total); k++) next();
  });
};

/* La lista de archivos (archivos.txt), para fs_prefetch_dir */
Module.d2list = null;
Module.d2loadList = function () {
  if (Module.d2list) return Promise.resolve(Module.d2list);
  return fetch('archivos.txt').then(function (r) { return r.ok ? r.text() : ''; }).then(function (t) {
    Module.d2list = t.split(/\r?\n/).map(function (s) { return s.trim().toLowerCase().replace(/\\/g, '/'); })
                     .filter(function (s) { return s && s[0] !== '#'; });
    return Module.d2list;
  });
};

/* fs_prefetch_dir: todo lo de esa carpeta (y sus subcarpetas), en segundo plano */
Module.d2prefetchDir = function (dir) {
  var prefix = dir.toLowerCase().replace(/\\/g, '/').replace(/\/$/, '') + '/';
  Module.d2loadList().then(function (list) {
    Module.d2prefetch(list.filter(function (s) { return s.indexOf(prefix) === 0; }), 8);
  });
};

/* Una lista de precarga (precarga.txt): las rutas, sin repetir */
Module.d2loadPrefetchList = function (name) {
  return fetch(name).then(function (r) { return r.ok ? r.text() : ''; }).then(function (t) {
    var seen = new Set(), out = [];
    t.split(/\r?\n/).forEach(function (s) {
      s = s.trim().toLowerCase().replace(/\\/g, '/');
      if (s && s[0] !== '#' && !seen.has(s)) { seen.add(s); out.push(s); }
    });
    return out;
  }, function () { return []; });
};
