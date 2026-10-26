/* La red local en el navegador (--pre-js, como cache.js; docs/web.md). El navegador no abre conexiones TCP ni
   UDP: src/net.c (con __EMSCRIPTEN__) pasa todo por d2port-servidor (tools/d2port-servidor.c) con WebSockets.
   Cada conexión del juego es un número (fd) de Module.d2net.socks:
   - unirse: un WebSocket a /red/conectar (el servidor abre la conexión TCP con la otra máquina);
   - crear una partida: un WebSocket a /red/anfitrion; el servidor escucha en el puerto y avisa de cada conexión
     que llega ([1][n]), lo que manda ([2][n][datos]) y cuando se va ([3][n]); acá cada una es un fd más (kid) y
     lo que se le manda va por el mismo WebSocket ([2][n][datos], [3][n] cortarla, [4]texto lo que se contesta a
     los que buscan, [5] ya no entra nadie);
   - buscar partidas: /red/buscar contesta lo que respondieron en medio segundo, un renglón por partida. */
Module.d2net = { next: 1, socks: {}, found: [], finding: false };

/* Abre el WebSocket (path: /red/...); da el fd, o -1 si el servidor no pudo (no está, no contesta esa IP, el
   puerto está ocupado) */
Module.d2netOpen = function (path, host) {
  var N = Module.d2net, url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + path;
  return new Promise(function (done) {
    var ws;
    try { ws = new WebSocket(url); } catch (e) { done(-1); return; }
    ws.binaryType = 'arraybuffer';
    var s = { ws: ws, q: [], off: 0, closed: false, host: host, kids: {}, accepts: [], said: '' };
    var timer = setTimeout(function () { try { ws.close(); } catch (e) {} done(-1); }, 5000);
    ws.onopen = function () { clearTimeout(timer); var fd = N.next++; N.socks[fd] = s; done(fd); };
    ws.onerror = ws.onclose = function () {
      clearTimeout(timer);
      s.closed = true;
      for (var k in s.kids) s.kids[k].closed = true;
      done(-1); /* (si ya había abierto, no cambia nada) */
    };
    ws.onmessage = function (e) {
      var d = new Uint8Array(e.data), kid;
      if (!s.host) { s.q.push(d); return; }
      if (d[0] === 1 && d.length >= 2) {
        kid = { parent: s, id: d[1], q: [], off: 0, closed: false };
        s.kids[d[1]] = kid;
        var fd = N.next++;
        N.socks[fd] = kid;
        s.accepts.push(fd);
      } else if (d[0] === 2 && (kid = s.kids[d[1]])) {
        kid.q.push(d.subarray(2));
      } else if (d[0] === 3 && (kid = s.kids[d[1]])) {
        kid.closed = true;
        delete s.kids[d[1]];
      }
    };
  });
};

Module.d2netGone = function (s) { return !s || s.closed || (s.parent && s.parent.closed); };

/* Manda los bytes (ya copiados): siempre entran todos (el WebSocket los guarda); 0 = la conexión se cortó */
Module.d2netSend = function (fd, bytes) {
  var s = Module.d2net.socks[fd];
  if (Module.d2netGone(s)) return 0;
  try {
    if (s.parent) {
      var m = new Uint8Array(bytes.length + 2);
      m[0] = 2;
      m[1] = s.id;
      m.set(bytes, 2);
      s.parent.ws.send(m);
    } else {
      s.ws.send(bytes);
    }
  } catch (e) { return 0; }
  return bytes.length;
};

Module.d2netClose = function (fd) {
  var N = Module.d2net, s = N.socks[fd];
  if (!s) return;
  delete N.socks[fd];
  if (s.parent) {
    if (!Module.d2netGone(s)) try { s.parent.ws.send(new Uint8Array([3, s.id])); } catch (e) {}
    delete s.parent.kids[s.id];
    return;
  }
  s.closed = true;
  for (var k in s.kids) s.kids[k].closed = true;
  s.accepts.forEach(function (a) { delete N.socks[a]; });
  try { s.ws.close(); } catch (e) {}
};

Module.d2netAccept = function (fd) {
  var s = Module.d2net.socks[fd];
  return s && s.accepts.length ? s.accepts.shift() : -1;
};

/* Lo que el servidor contesta a los que buscan partidas ("D2PN!jugadores modo número"), solo si cambió */
Module.d2netSay = function (fd, text) {
  var s = Module.d2net.socks[fd];
  if (Module.d2netGone(s) || s.said === text) return;
  s.said = text;
  try { s.ws.send(new Uint8Array([4].concat(Array.from(text, function (c) { return c.charCodeAt(0) & 255; })))); } catch (e) {}
};

Module.d2netStop = function (fd) {
  var s = Module.d2net.socks[fd];
  if (!Module.d2netGone(s)) try { s.ws.send(new Uint8Array([5])); } catch (e) {}
};

/* Buscar: una pregunta a la vez; las respuestas quedan en found para net_discover_poll */
Module.d2netFind = function (port) {
  var N = Module.d2net;
  if (N.finding) return;
  N.finding = true;
  fetch('/red/buscar?p=' + port, { cache: 'no-store' }).then(function (r) { return r.ok ? r.text() : ''; })
    .catch(function () { return ''; }).then(function (t) {
      N.finding = false;
      t.split('\n').forEach(function (l) {
        var i = l.indexOf('\t');
        if (i > 0) N.found.push([l.slice(0, i), l.slice(i + 1)]);
      });
    });
};
