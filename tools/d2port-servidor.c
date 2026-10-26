/* d2port-servidor: el servidor de la versión para el navegador (docs/web.md). Va en la carpeta de index.html y
   se abre con doble clic; solo atiende a esta computadora (escucha en 127.0.0.1) y hace tres cosas:
   - sirve la página y los archivos del juego (data/) al navegador, que no carga WebAssembly de un archivo suelto;
   - guarda las partidas en save/ de esta carpeta, como la versión de PC (la página las lee al empezar y sube las
     que cambian: GET /partidas, PUT y DELETE /save/...), en vez de dejarlas en el navegador;
   - hace de puente de la red local (src/net.c): el navegador no abre conexiones TCP ni UDP, así que se une a una
     partida por /red/conectar (un WebSocket que acá pasa a TCP), crea una por /red/anfitrion (acá se escucha en
     el puerto 6112 por la página: cada conexión que llega va y viene por ese WebSocket) y busca por /red/buscar
     (la pregunta UDP a toda la red).
   Solo la página de este servidor puede usar lo que cambia algo o abre conexiones (se revisa el Origin) y solo
   con su nombre (el Host: sin trucos de DNS). Uso: d2port-servidor [--puerto N] [--sin-navegador] [--prueba]
   (PORT=N y D2_NO_BROWSER=1 también). Se compila con scripts/build-web.sh (Linux y Windows). */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define __USE_MINGW_ANSI_STDIO 1
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#include <sys/stat.h>
typedef SOCKET sock_t;
#define sock_close closesocket
#define MSG_NOSIGNAL 0
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define sock_close close
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef D2_VERSION
#define D2_VERSION "dev"
#endif
#define NET_PORT  6112      /* el de la red local del juego (src/net.h) */
#define WEB_PORT  8642      /* el de la página */
#define BODY_MAX  (64 << 20) /* una partida subida, un mensaje del navegador: como el tope de src/net.c */
#define HOST_MAX  16        /* conexiones que llegan a una partida del navegador (el juego acepta 7) */

static int s_es;   /* los mensajes en español (la computadora está en español); si no, en inglés */
static int s_port; /* el de la página: el Host y el Origin tienen que ser este servidor */
#define T(es, en) (s_es ? (es) : (en))

/* ---- Lo de cada sistema ---- */

typedef struct {
    void (*fn)(void *);
    void *arg;
} Job;

#ifdef _WIN32
static DWORD WINAPI job_run(LPVOID p)
#else
static void *job_run(void *p)
#endif
{
    Job j = *(Job *)p;
    free(p);
    j.fn(j.arg);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Un hilo por conexión (cada una espera lo suyo sin trabar a las otras) */
static int go(void (*fn)(void *), void *arg)
{
    Job *j = malloc(sizeof *j);
    if (!j)
        return -1;
    j->fn = fn;
    j->arg = arg;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, job_run, j, 0, NULL);
    if (!h) {
        free(j);
        return -1;
    }
    CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, job_run, j) != 0) {
        free(j);
        return -1;
    }
    pthread_detach(t);
#endif
    return 0;
}

static long long now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
#endif
}

/* ¿Es un archivo (no una carpeta ni un dispositivo)? Con su tamaño */
static int file_info(const char *path, long long *size)
{
#ifdef _WIN32
    struct __stat64 st;
    if (_stat64(path, &st) != 0 || !(st.st_mode & _S_IFREG))
        return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
#endif
    if (size)
        *size = (long long)st.st_size;
    return 1;
}

static void make_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

/* Reemplaza el archivo de una vez (si se corta a la mitad, queda la partida de antes) */
static int replace_file(const char *tmp, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(tmp, path);
#endif
}

static void open_browser(const char *url)
{
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    if (fork() == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        _exit(1);
    }
#endif
}

static void set_nodelay(sock_t s)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one); /* (los controles de la red, ya) */
}

static void set_nonblock(sock_t s, int on)
{
#ifdef _WIN32
    u_long v = (u_long)on;
    ioctlsocket(s, FIONBIO, &v);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
#endif
}

static void set_recv_timeout(sock_t s, int seconds)
{
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv = { seconds, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

static int send_all(sock_t s, const void *b, size_t n)
{
    const char *p = b;
    while (n > 0) {
        int k = send(s, p, n > (1u << 20) ? 1 << 20 : (int)n, MSG_NOSIGNAL);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int grow(uint8_t **b, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    size_t c = *cap ? *cap : 65536;
    while (c < need)
        c *= 2;
    uint8_t *nb = realloc(*b, c);
    if (!nb)
        return -1;
    *b = nb;
    *cap = c;
    return 0;
}

/* ---- La red local (lo mismo que src/net.c) ---- */

/* D2_NET_BIND=ip (pruebas): escuchar las partidas solo en esa dirección; si no, en todas */
static uint32_t bind_addr(void)
{
    struct in_addr in;
    const char *b = getenv("D2_NET_BIND");
    return b && inet_pton(AF_INET, b, &in) == 1 ? in.s_addr : htonl(INADDR_ANY);
}

/* Las direcciones de difusión de cada red de esta máquina (como bcast_addrs de src/net.c) */
static int bcast_addrs(uint32_t *out, int max)
{
    int n = 0;
#ifdef _WIN32
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = malloc(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = list ? GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) : ERROR_NOT_ENOUGH_MEMORY;
    if (r == ERROR_BUFFER_OVERFLOW) {
        free(list);
        list = malloc(size);
        r = list ? GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) : ERROR_NOT_ENOUGH_MEMORY;
    }
    for (IP_ADAPTER_ADDRESSES *ad = r == NO_ERROR ? list : NULL; ad && n < max; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = ad->FirstUnicastAddress; u && n < max; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET || u->OnLinkPrefixLength == 0 || u->OnLinkPrefixLength > 30)
                continue;
            uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
            uint32_t mask = 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength);
            out[n++] = htonl(ip | ~mask);
        }
    }
    free(list);
#else
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) == 0)
        for (struct ifaddrs *i = list; i && n < max; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr)
                out[n++] = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr;
    freeifaddrs(list);
#endif
    return n;
}

/* La IP de esta máquina en la red (la que se muestra al crear una partida; como net_my_ip de src/net.c) */
static void my_ip(char *ip, size_t cap)
{
    ip[0] = '\0';
#ifdef _WIN32
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return;
    struct sockaddr_in to, me;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);
    int ml = sizeof me;
    if (connect(s, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(s, (struct sockaddr *)&me, &ml) == 0)
        inet_ntop(AF_INET, &me.sin_addr, ip, cap);
    sock_close(s);
#else
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0)
        return;
    for (struct ifaddrs *i = list; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "lo") != 0) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, cap);
            break;
        }
    freeifaddrs(list);
#endif
}

/* Conectarse por TCP sin esperar más de 3 segundos si no hay nadie */
static sock_t tcp_connect(struct in_addr ip, int port)
{
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return s;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr = ip;
    set_nonblock(s, 1);
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    if (!ok) {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        struct timeval tv = { 3, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        ok = select((int)s + 1, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w) &&
             getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) == 0 && err == 0;
    }
    if (!ok) {
        sock_close(s);
        return INVALID_SOCKET;
    }
    set_nonblock(s, 0);
    set_nodelay(s);
    return s;
}

/* ---- SHA-1 y base64 (el saludo del WebSocket) ---- */

static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t *m = calloc(total, 1);
    if (!m) {
        memset(out, 0, 20);
        return;
    }
    memcpy(m, msg, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        m[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 |
                   m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)
                f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40)
                f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60)
                f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else
                f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++)
            out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
}

static void base64(const uint8_t *in, size_t n, char *out)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = abc[v >> 18 & 63];
        out[o++] = abc[v >> 12 & 63];
        out[o++] = i + 1 < n ? abc[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? abc[v & 63] : '=';
    }
    out[o] = '\0';
}

/* Sec-WebSocket-Accept: base64(SHA-1(la clave + el número del protocolo)) */
static void ws_accept_key(const char *key, char out[32])
{
    char both[128];
    uint8_t h[20];
    int n = snprintf(both, sizeof both, "%.60s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1((const uint8_t *)both, (size_t)n, h);
    base64(h, 20, out);
}

/* ---- HTTP ---- */

typedef struct {
    sock_t fd;
    char   buf[16384]; /* lo que llegó y falta leer */
    size_t len;
} Conn;

typedef struct {
    char      method[8], path[1024], query[512];
    char      host[128], origin[160], upgrade[32], wskey[64];
    long long length;
    int       close;
} Req;

static int eqi(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
    return 1;
}

static void copy_trim(char *dst, size_t cap, const char *s, size_t n)
{
    while (n && (*s == ' ' || *s == '\t'))
        s++, n--;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* El pedido escrito (la primera línea y los encabezados, hasta la línea vacía): 1 = se entendió, -1 = no */
static int parse_req(const char *h, size_t n, Req *r)
{
    memset(r, 0, sizeof *r);
    const char *end = h + n, *eol = memchr(h, '\n', n);
    if (!eol)
        return -1;
    const char *sp1 = memchr(h, ' ', (size_t)(eol - h));
    const char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
    if (!sp1 || !sp2 || sp1 - h >= (int)sizeof r->method)
        return -1;
    memcpy(r->method, h, (size_t)(sp1 - h));
    const char *target = sp1 + 1, *q = memchr(target, '?', (size_t)(sp2 - target));
    const char *pend = q ? q : sp2;
    if (pend - target >= (int)sizeof r->path || (q && sp2 - q - 1 >= (int)sizeof r->query) || *target != '/')
        return -1;
    memcpy(r->path, target, (size_t)(pend - target));
    if (q)
        memcpy(r->query, q + 1, (size_t)(sp2 - q - 1));
    r->close = eqi(sp2 + 1, "HTTP/1.0", 8);
    for (const char *line = eol + 1; line < end;) {
        const char *le = memchr(line, '\n', (size_t)(end - line));
        if (!le)
            le = end;
        const char *colon = memchr(line, ':', (size_t)(le - line));
        size_t vn = colon ? (size_t)(le - colon - 1) : 0;
        if (vn && colon[vn] == '\r') /* (colon[vn] es el último carácter del valor) */
            vn--;
        if (colon) {
            size_t kn = (size_t)(colon - line);
            const char *v = colon + 1;
            char num[32];
            if (kn == 4 && eqi(line, "Host", 4))
                copy_trim(r->host, sizeof r->host, v, vn);
            else if (kn == 6 && eqi(line, "Origin", 6))
                copy_trim(r->origin, sizeof r->origin, v, vn);
            else if (kn == 7 && eqi(line, "Upgrade", 7))
                copy_trim(r->upgrade, sizeof r->upgrade, v, vn);
            else if (kn == 17 && eqi(line, "Sec-WebSocket-Key", 17))
                copy_trim(r->wskey, sizeof r->wskey, v, vn);
            else if (kn == 14 && eqi(line, "Content-Length", 14)) {
                copy_trim(num, sizeof num, v, vn);
                r->length = strtoll(num, NULL, 10);
                if (r->length < 0)
                    return -1;
            } else if (kn == 10 && eqi(line, "Connection", 10)) {
                char c[64];
                copy_trim(c, sizeof c, v, vn);
                if (eqi(c, "close", 6))
                    r->close = 1;
            } else if (kn == 17 && eqi(line, "Transfer-Encoding", 17))
                return -1; /* (la página siempre manda el largo) */
        }
        line = le + 1;
    }
    return 1;
}

static const char *find_end(const char *b, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n')
            return b + i + 4;
    return NULL;
}

/* El próximo pedido de la conexión: 1 = listo, 0 = se cortó, -1 = no se entiende */
static int read_req(Conn *c, Req *r)
{
    const char *end;
    while (!(end = find_end(c->buf, c->len))) {
        if (c->len >= sizeof c->buf)
            return -1;
        int k = recv(c->fd, c->buf + c->len, (int)(sizeof c->buf - c->len), 0);
        if (k <= 0)
            return 0;
        c->len += (size_t)k;
    }
    size_t hn = (size_t)(end - c->buf);
    int ok = parse_req(c->buf, hn - 2, r);
    memmove(c->buf, c->buf + hn, c->len - hn);
    c->len -= hn;
    return ok;
}

/* El cuerpo del pedido (lo que ya llegó con los encabezados y el resto): out = NULL lo tira */
static int read_body(Conn *c, long long n, uint8_t *out)
{
    long long got = 0;
    size_t first = c->len < (size_t)n ? c->len : (size_t)n;
    if (out)
        memcpy(out, c->buf, first);
    memmove(c->buf, c->buf + first, c->len - first);
    c->len -= first;
    got = (long long)first;
    while (got < n) {
        char tmp[65536];
        long long want = n - got < (long long)sizeof tmp ? n - got : (long long)sizeof tmp;
        int k = recv(c->fd, out ? (char *)out + got : tmp, (int)want, 0);
        if (k <= 0)
            return -1;
        got += k;
    }
    return 0;
}

static void reply(Conn *c, int code, const char *status, const char *type, const void *body, size_t n)
{
    char h[512];
    int hn = snprintf(h, sizeof h,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nCache-Control: no-cache\r\n\r\n",
                      code, status, type, (unsigned long long)n);
    if (send_all(c->fd, h, (size_t)hn) == 0 && n)
        send_all(c->fd, body, n);
}

static void reply_text(Conn *c, int code, const char *status, const char *text)
{
    reply(c, code, status, "text/plain; charset=utf-8", text, strlen(text));
}

/* El nombre con que se pidió (Host) o la página que pide (Origin): este servidor, por cualquiera de sus nombres */
static int is_me(const char *s, const char *scheme)
{
    static const char *names[] = { "localhost", "127.0.0.1", "[::1]" };
    for (int i = 0; i < 3; i++) {
        char me[96];
        int n = snprintf(me, sizeof me, "%s%s:%d", scheme, names[i], s_port);
        if (strlen(s) == (size_t)n && eqi(s, me, (size_t)n))
            return 1;
    }
    return 0;
}

/* Lo que cambia algo o abre conexiones: sin Origin es un programa de esta computadora (curl, una prueba); con,
   tiene que ser la página de este servidor (otra página abierta en el navegador no puede) */
static int origin_ok(const Req *r)
{
    return !r->origin[0] || is_me(r->origin, "http://");
}

static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* La ruta pedida como archivo de esta carpeta (sin %xx): nada de "..", "\" ni ":" (nada afuera de la carpeta, ni
   otra unidad en Windows). 0 = no sirve */
static int safe_path(const char *url, char *out, size_t cap)
{
    size_t n = 0;
    while (*url == '/')
        url++;
    for (const char *p = url; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch == '%') {
            int a = hexv(p[1]), b = a >= 0 ? hexv(p[2]) : -1;
            if (b < 0)
                return 0;
            ch = a * 16 + b;
            p += 2;
        }
        if (ch < 32 || ch == 127 || ch == '\\' || ch == ':' || n + 1 >= cap)
            return 0;
        out[n++] = (char)ch;
    }
    out[n] = '\0';
    return out[0] != '/' && !strstr(out, "..");
}

/* Un archivo de save/: solo letras, números, ".", "_", "-" y "/" (los nombres que usa el juego) */
static int save_name_ok(const char *rel)
{
    if (!rel[0] || rel[0] == '/' || strstr(rel, "..") || strlen(rel) > 200)
        return 0;
    for (const char *p = rel; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' ||
              *p == '_' || *p == '-' || *p == '/'))
            return 0;
    return 1;
}

static const char *query_get(const char *q, const char *key, char *out, size_t cap)
{
    size_t kn = strlen(key);
    out[0] = '\0';
    for (const char *p = q; *p;) {
        const char *amp = strchr(p, '&'), *e = amp ? amp : p + strlen(p);
        if ((size_t)(e - p) > kn && !strncmp(p, key, kn) && p[kn] == '=') {
            size_t n = (size_t)(e - p - kn - 1);
            if (n >= cap)
                n = cap - 1;
            memcpy(out, p + kn + 1, n);
            out[n] = '\0';
            break;
        }
        p = amp ? amp + 1 : e;
    }
    return out;
}

static const char *mime(const char *path)
{
    const char *e = strrchr(path, '.');
    static const char *types[][2] = {
        { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript" }, { ".wasm", "application/wasm" },
        { ".txt", "text/plain; charset=utf-8" }, { ".json", "application/json" }, { ".css", "text/css" },
        { ".png", "image/png" }, { ".ico", "image/x-icon" },
    };
    for (size_t i = 0; e && i < sizeof types / sizeof types[0]; i++)
        if (eqi(e, types[i][0], strlen(types[i][0]) + 1))
            return types[i][1];
    return "application/octet-stream";
}

static void serve_file(Conn *c, const Req *r)
{
    char rel[1024];
    long long size = 0;
    if (!safe_path(r->path, rel, sizeof rel)) {
        reply_text(c, 403, "Forbidden", "");
        return;
    }
    if (!rel[0])
        snprintf(rel, sizeof rel, "index.html");
    FILE *f = file_info(rel, &size) ? fopen(rel, "rb") : NULL;
    if (!f) {
        reply_text(c, 404, "Not Found", "");
        return;
    }
    char h[512];
    int hn = snprintf(h, sizeof h,
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nCache-Control: no-cache\r\n\r\n",
                      mime(rel), size);
    int ok = send_all(c->fd, h, (size_t)hn) == 0;
    if (ok && strcmp(r->method, "HEAD") != 0) {
        char *b = malloc(1 << 16);
        size_t k;
        while (b && ok && (k = fread(b, 1, 1 << 16, f)) > 0)
            ok = send_all(c->fd, b, k) == 0;
        free(b);
    }
    fclose(f);
}

/* ---- Las partidas (save/) ---- */

static void list_dir(const char *dir, const char *rel, uint8_t **out, size_t *len, size_t *cap, int depth)
{
    char path[600], sub[400];
    if (depth > 4)
        return;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof path, "%s/*", dir);
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const char *name = fd.cFileName;
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (stat(path, &st) != 0)
            continue;
        int is_dir = S_ISDIR(st.st_mode);
#endif
        size_t nl = strlen(name);
        if (name[0] != '.' && !(nl > 4 && !strcmp(name + nl - 4, ".tmp"))) {
            snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", name);
            snprintf(path, sizeof path, "%s/%s", dir, name);
            if (is_dir)
                list_dir(path, sub, out, len, cap, depth + 1);
            else if (save_name_ok(sub) && grow(out, cap, *len + strlen(sub) + 2) == 0) {
                memcpy(*out + *len, sub, strlen(sub));
                *len += strlen(sub);
                (*out)[(*len)++] = '\n';
            }
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
}

static void save_list(Conn *c)
{
    uint8_t *out = NULL;
    size_t len = 0, cap = 0;
    list_dir("save", "", &out, &len, &cap, 0);
    reply(c, 200, "OK", "text/plain; charset=utf-8", out, len);
    free(out);
}

static int save_put(Conn *c, const Req *r, const char *rel)
{
    if (r->length > BODY_MAX) {
        reply_text(c, 413, "Payload Too Large", "");
        return -1;
    }
    uint8_t *body = malloc((size_t)r->length + 1);
    if (!body || read_body(c, r->length, body) != 0) {
        free(body);
        return -1;
    }
    char path[300], tmp[310];
    snprintf(path, sizeof path, "save/%.200s", rel); /* (save_name_ok: hasta 200) */
    for (char *p = strchr(path, '/'); p; p = strchr(p + 1, '/')) { /* (las carpetas que falten) */
        *p = '\0';
        make_dir(path);
        *p = '/';
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(body, 1, (size_t)r->length, f) == (size_t)r->length;
    if (f)
        ok = fclose(f) == 0 && ok;
    ok = ok && replace_file(tmp, path) == 0;
    if (!ok) {
        remove(tmp);
        fprintf(stderr, "%s save/%s\n", T("No pude guardar", "Could not save"), rel);
    }
    free(body);
    reply_text(c, ok ? 204 : 500, ok ? "No Content" : "Internal Server Error", "");
    return 0;
}

/* ---- WebSocket ---- */

typedef struct {
    sock_t   fd;
    uint8_t *in, *msg;
    size_t   in_len, in_cap, msg_len, msg_cap;
    int      msg_op, msg_done;
} Ws;

static int ws_send(sock_t fd, int op, const uint8_t *pre, size_t pn, const void *d, size_t n)
{
    size_t total = pn + n, hn = total < 126 ? 2 : total < 65536 ? 4 : 10;
    uint8_t *b = malloc(hn + total);
    if (!b)
        return -1;
    b[0] = (uint8_t)(0x80 | op);
    if (hn == 2) {
        b[1] = (uint8_t)total;
    } else if (hn == 4) {
        b[1] = 126;
        b[2] = (uint8_t)(total >> 8);
        b[3] = (uint8_t)total;
    } else {
        b[1] = 127;
        for (int i = 0; i < 8; i++)
            b[2 + i] = (uint8_t)((uint64_t)total >> (56 - 8 * i));
    }
    if (pn)
        memcpy(b + hn, pre, pn);
    if (n)
        memcpy(b + hn + pn, d, n);
    int r = send_all(fd, b, hn + total);
    free(b);
    return r;
}

/* Lo que llegó del navegador (select dijo que hay): -1 = se cortó */
static int ws_fill(Ws *w)
{
    if (grow(&w->in, &w->in_cap, w->in_len + 65536))
        return -1;
    int k = recv(w->fd, (char *)w->in + w->in_len, 65536, 0);
    if (k <= 0)
        return -1;
    w->in_len += (size_t)k;
    return 0;
}

/* El próximo mensaje entero del navegador: 1 = hay (*op 1 texto o 2 binario en w->msg, u 8 = cerrar), 0 = falta
   que llegue más, -1 = no se entiende. Contesta los ping; junta los mensajes partidos en varios pedazos */
static int ws_next(Ws *w, int *op)
{
    for (;;) {
        uint8_t *b = w->in;
        if (w->in_len < 2)
            return 0;
        int fin = b[0] & 0x80, opc = b[0] & 15;
        uint64_t n = b[1] & 127;
        size_t h = 2;
        if (!(b[1] & 0x80))
            return -1; /* (lo del navegador siempre viene con máscara) */
        if (n == 126) {
            if (w->in_len < 4)
                return 0;
            n = (uint64_t)b[2] << 8 | b[3];
            h = 4;
        } else if (n == 127) {
            if (w->in_len < 10)
                return 0;
            n = 0;
            for (int i = 0; i < 8; i++)
                n = n << 8 | b[2 + i];
            h = 10;
        }
        if (n > BODY_MAX)
            return -1;
        if (w->in_len < h + 4 + n)
            return 0;
        uint8_t *mk = b + h, *p = b + h + 4;
        for (uint64_t i = 0; i < n; i++)
            p[i] ^= mk[i & 3];
        int deliver = 0;
        if (opc == 8) {
            *op = 8;
            deliver = 1;
        } else if (opc == 9) {
            ws_send(w->fd, 10, NULL, 0, p, (size_t)n);
        } else if (opc < 8) {
            if (opc != 0 || w->msg_done) {
                w->msg_len = 0;
                w->msg_op = opc ? opc : 2;
            }
            w->msg_done = 0;
            if (grow(&w->msg, &w->msg_cap, w->msg_len + (size_t)n + 1))
                return -1;
            memcpy(w->msg + w->msg_len, p, (size_t)n);
            w->msg_len += (size_t)n;
            if (fin) {
                *op = w->msg_op;
                w->msg_done = deliver = 1;
            }
        }
        size_t used = h + 4 + (size_t)n;
        memmove(w->in, w->in + used, w->in_len - used);
        w->in_len -= used;
        if (deliver)
            return 1;
    }
}

/* El saludo: de acá en adelante la conexión es un WebSocket (lo que ya llegó después del pedido pasa a w) */
static int ws_begin(Conn *c, const Req *r, Ws *w)
{
    char key[32], h[256];
    memset(w, 0, sizeof *w);
    w->fd = c->fd;
    if (c->len) {
        if (grow(&w->in, &w->in_cap, c->len))
            return -1;
        memcpy(w->in, c->buf, c->len);
    }
    w->in_len = c->len;
    c->len = 0;
    ws_accept_key(r->wskey, key);
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     key);
    set_nodelay(c->fd);
    return send_all(c->fd, h, (size_t)n);
}

static void ws_end(Ws *w)
{
    ws_send(w->fd, 8, NULL, 0, NULL, 0);
    free(w->in);
    free(w->msg);
}

static int port_of(const Req *r)
{
    char ps[16];
    int port = atoi(query_get(r->query, "p", ps, sizeof ps));
    return port ? port : NET_PORT;
}

/* /red/conectar?a=IP&p=puerto: el navegador se une a una partida; acá se abre la conexión TCP y se pasa todo tal
   cual en las dos direcciones (los mensajes de src/net.c van adentro, sin cambiar nada) */
static void red_conectar(Conn *c, const Req *r)
{
    char a[64];
    struct in_addr ip;
    int port = port_of(r);
    query_get(r->query, "a", a, sizeof a);
    if (!strcmp(a, "localhost"))
        snprintf(a, sizeof a, "127.0.0.1");
    if (inet_pton(AF_INET, a, &ip) != 1 || port < 1024 || port > 65535) {
        reply_text(c, 400, "Bad Request", "");
        return;
    }
    sock_t t = tcp_connect(ip, port);
    if (t == INVALID_SOCKET) {
        printf("%s %s:%d\n", T("red: no contesta", "network: no answer from"), a, port);
        reply_text(c, 502, "Bad Gateway", "");
        return;
    }
    Ws w;
    uint8_t *buf = malloc(65536);
    if (buf && ws_begin(c, r, &w) == 0) {
        printf("%s %s\n", T("red: el navegador se unió a la partida de", "network: the browser joined the game at"), a);
        for (;;) {
            int op, k = 0;
            while ((k = ws_next(&w, &op)) == 1 && op != 8)
                if (send_all(t, w.msg, w.msg_len) != 0)
                    break;
            if (k != 0)
                break;
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(c->fd, &rs);
            FD_SET(t, &rs);
            if (select((int)(c->fd > t ? c->fd : t) + 1, &rs, NULL, NULL, NULL) < 0)
                break;
            if (FD_ISSET(t, &rs)) {
                int got = recv(t, (char *)buf, 65536, 0);
                if (got <= 0 || ws_send(c->fd, 2, NULL, 0, buf, (size_t)got) != 0)
                    break;
            }
            if (FD_ISSET(c->fd, &rs) && ws_fill(&w) != 0)
                break;
        }
        ws_end(&w);
        printf("%s %s\n", T("red: terminó la conexión con", "network: connection ended with"), a);
    }
    free(buf);
    sock_close(t);
}

/* /red/anfitrion?p=puerto: el navegador crea una partida. Acá se escucha en el puerto (TCP para unirse, UDP para
   contestar a los que buscan) y cada conexión va y viene por este WebSocket, con un número. Lo que se manda:
     acá → página: [1][n] llegó la conexión n, [2][n][datos] lo que mandó, [3][n] se fue
     página → acá: [2][n][datos] mandarle, [3][n] cortarla, [4][texto] la respuesta a los que buscan ("D2PN!..."),
                   [5] ya no entra nadie (empezó la partida) */
static void red_anfitrion(Conn *c, const Req *r)
{
    int port = port_of(r), one = 1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = bind_addr();
    a.sin_port = htons((uint16_t)port);
    sock_t l = port >= 1024 && port <= 65535 ? socket(AF_INET, SOCK_STREAM, 0) : INVALID_SOCKET;
#ifndef _WIN32
    if (l != INVALID_SOCKET)
        setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
    if (l == INVALID_SOCKET || bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 8) != 0) {
        printf("%s %d\n", T("red: no pude escuchar en el puerto", "network: could not listen on port"), port);
        if (l != INVALID_SOCKET)
            sock_close(l);
        reply_text(c, 409, "Conflict", "");
        return;
    }
    sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u != INVALID_SOCKET) {
        setsockopt(u, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        if (bind(u, (struct sockaddr *)&a, sizeof a) != 0) {
            sock_close(u);
            u = INVALID_SOCKET;
        }
    }
    struct {
        sock_t s;
        int    id;
    } peer[HOST_MAX];
    for (int i = 0; i < HOST_MAX; i++)
        peer[i].s = INVALID_SOCKET;
    char say[64] = "", ip[64];
    int next_id = 1;
    Ws w;
    uint8_t *buf = malloc(65536);
    if (buf && ws_begin(c, r, &w) == 0) {
        my_ip(ip, sizeof ip);
        printf("%s %d (%s)\n", T("red: el navegador creó una partida en el puerto", "network: the browser hosts a game on port"),
               port, ip);
        for (;;) {
            int op, k = 0, quit = 0;
            while (!quit && (k = ws_next(&w, &op)) == 1) {
                const uint8_t *m = w.msg;
                size_t n = w.msg_len;
                int at = -1;
                if (op == 8) {
                    quit = 1;
                    break;
                }
                for (int i = 0; n >= 2 && i < HOST_MAX; i++)
                    if (peer[i].s != INVALID_SOCKET && peer[i].id == m[1])
                        at = i;
                if (n >= 2 && m[0] == 2 && at >= 0 && send_all(peer[at].s, m + 2, n - 2) != 0) {
                    uint8_t gone[2] = { 3, (uint8_t)peer[at].id };
                    sock_close(peer[at].s);
                    peer[at].s = INVALID_SOCKET;
                    quit = ws_send(c->fd, 2, gone, 2, NULL, 0) != 0;
                } else if (n >= 2 && m[0] == 3 && at >= 0) {
                    sock_close(peer[at].s);
                    peer[at].s = INVALID_SOCKET;
                } else if (n >= 1 && m[0] == 4) {
                    size_t tn = n - 1 < sizeof say - 1 ? n - 1 : sizeof say - 1;
                    memcpy(say, m + 1, tn);
                    say[tn] = '\0';
                } else if (n >= 1 && m[0] == 5) {
                    if (l != INVALID_SOCKET)
                        sock_close(l);
                    if (u != INVALID_SOCKET)
                        sock_close(u);
                    l = u = INVALID_SOCKET;
                }
            }
            if (quit || k < 0)
                break;
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(c->fd, &rs);
            sock_t mx = c->fd;
#define ADD(s)                     \
    do {                           \
        if ((s) != INVALID_SOCKET) { \
            FD_SET((s), &rs);      \
            if ((s) > mx)          \
                mx = (s);          \
        }                          \
    } while (0)
            ADD(l);
            ADD(u);
            for (int i = 0; i < HOST_MAX; i++)
                ADD(peer[i].s);
#undef ADD
            if (select((int)mx + 1, &rs, NULL, NULL, NULL) < 0)
                break;
            if (l != INVALID_SOCKET && FD_ISSET(l, &rs)) {
                sock_t s = accept(l, NULL, NULL);
                int at = -1;
                for (int i = 0; i < HOST_MAX && at < 0; i++)
                    if (peer[i].s == INVALID_SOCKET)
                        at = i;
                if (s != INVALID_SOCKET && at >= 0) {
                    /* (números que no se repiten enseguida: lo que la página mandó a una que se fue no le llega a la
                       que entró después) */
                    for (int tries = 0; tries < 256; tries++) {
                        int used = 0;
                        for (int i = 0; i < HOST_MAX; i++)
                            used |= peer[i].s != INVALID_SOCKET && peer[i].id == next_id;
                        if (!used)
                            break;
                        next_id = next_id % 255 + 1;
                    }
                    uint8_t hi[2] = { 1, (uint8_t)next_id };
                    set_nodelay(s);
                    peer[at].s = s;
                    peer[at].id = next_id;
                    next_id = next_id % 255 + 1;
                    if (ws_send(c->fd, 2, hi, 2, NULL, 0) != 0)
                        break;
                } else if (s != INVALID_SOCKET) {
                    sock_close(s);
                }
            }
            if (u != INVALID_SOCKET && FD_ISSET(u, &rs)) { /* "¿hay partidas?" */
                char q[16];
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                int got = recvfrom(u, q, (int)sizeof q, 0, (struct sockaddr *)&from, &fl);
                if (got >= 5 && !memcmp(q, "D2PN?", 5) && say[0])
                    sendto(u, say, (int)strlen(say), 0, (struct sockaddr *)&from, fl);
            }
            int bad = 0;
            for (int i = 0; i < HOST_MAX && !bad; i++) {
                if (peer[i].s == INVALID_SOCKET || !FD_ISSET(peer[i].s, &rs))
                    continue;
                uint8_t pre[2] = { 2, (uint8_t)peer[i].id };
                int got = recv(peer[i].s, (char *)buf, 65536, 0);
                if (got > 0) {
                    bad = ws_send(c->fd, 2, pre, 2, buf, (size_t)got) != 0;
                } else {
                    pre[0] = 3;
                    sock_close(peer[i].s);
                    peer[i].s = INVALID_SOCKET;
                    bad = ws_send(c->fd, 2, pre, 2, NULL, 0) != 0;
                }
            }
            if (bad || (FD_ISSET(c->fd, &rs) && ws_fill(&w) != 0))
                break;
        }
        ws_end(&w);
        printf("%s\n", T("red: terminó la partida del navegador", "network: the browser's game ended"));
    }
    free(buf);
    for (int i = 0; i < HOST_MAX; i++)
        if (peer[i].s != INVALID_SOCKET)
            sock_close(peer[i].s);
    if (l != INVALID_SOCKET)
        sock_close(l);
    if (u != INVALID_SOCKET)
        sock_close(u);
}

/* /red/buscar?p=puerto: la pregunta "¿hay partidas?" a toda la red (como net_discover_start de src/net.c) y las
   respuestas de medio segundo, una por renglón: "IP<tab>D2PN!jugadores modo número" */
static void red_buscar(Conn *c, const Req *r)
{
    int port = port_of(r), one = 1;
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    uint8_t *out = NULL;
    size_t len = 0, cap = 0;
    if (s != INVALID_SOCKET) {
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
        uint32_t to[18];
        int n = bcast_addrs(to, 16);
        to[n++] = htonl(INADDR_BROADCAST);
        to[n++] = htonl(INADDR_LOOPBACK);
        for (int i = 0; i < n; i++) {
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)port);
            a.sin_addr.s_addr = to[i];
            sendto(s, "D2PN?", 5, 0, (struct sockaddr *)&a, sizeof a);
        }
        long long end = now_ms() + 500;
        for (long long left; (left = end - now_ms()) > 0;) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(s, &rs);
            struct timeval tv = { 0, (long)left * 1000 };
            if (select((int)s + 1, &rs, NULL, NULL, &tv) <= 0)
                break;
            char a[64], ip[64], line[160];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int k = recvfrom(s, a, (int)sizeof a - 1, 0, (struct sockaddr *)&from, &fl);
            if (k < 5) /* (en Windows, un "no hay nadie" de 127.0.0.1 llega como error: se sigue esperando) */
                continue;
            a[k] = '\0';
            if (memcmp(a, "D2PN!", 5) != 0 || strchr(a, '\n') || strchr(a, '\t'))
                continue;
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            int ln = snprintf(line, sizeof line, "%s\t%s\n", ip, a);
            if (ln > 0 && grow(&out, &cap, len + (size_t)ln) == 0) {
                memcpy(out + len, line, (size_t)ln);
                len += (size_t)ln;
            }
        }
        sock_close(s);
    }
    reply(c, 200, "OK", "text/plain; charset=utf-8", out, len);
    free(out);
}

/* ---- Cada conexión del navegador ---- */

static void serve(void *arg)
{
    Conn *c = arg;
    Req r;
    set_recv_timeout(c->fd, 120);
    for (;;) {
        int k = read_req(c, &r);
        if (k <= 0) {
            if (k < 0)
                reply_text(c, 400, "Bad Request", "");
            break;
        }
        if (r.host[0] && !is_me(r.host, "")) {
            reply_text(c, 403, "Forbidden", "");
            break;
        }
        int get = !strcmp(r.method, "GET") || !strcmp(r.method, "HEAD");
        if (r.upgrade[0]) {
            if (!get || !eqi(r.upgrade, "websocket", 10) || !r.wskey[0] || !origin_ok(&r))
                reply_text(c, 403, "Forbidden", "");
            else if (!strcmp(r.path, "/red/conectar"))
                red_conectar(c, &r);
            else if (!strcmp(r.path, "/red/anfitrion"))
                red_anfitrion(c, &r);
            else
                reply_text(c, 404, "Not Found", "");
            break; /* (después de un WebSocket, la conexión ya no sirve para pedidos) */
        }
        if (strcmp(r.method, "PUT") != 0 && r.length > 0 && (r.length > BODY_MAX || read_body(c, r.length, NULL) != 0))
            break;
        const char *rel = !strncmp(r.path, "/save/", 6) ? r.path + 6 : NULL;
        if (get && !strcmp(r.path, "/servidor")) {
            char ip[64], body[256];
            my_ip(ip, sizeof ip);
            int n = snprintf(body, sizeof body, "{\"servidor\":\"d2port-servidor\",\"version\":\"%s\",\"ip\":\"%s\"}", D2_VERSION, ip);
            reply(c, 200, "OK", "application/json", body, (size_t)n);
        } else if (get && !strcmp(r.path, "/partidas")) {
            save_list(c);
        } else if (get && !strcmp(r.path, "/red/buscar")) {
            if (origin_ok(&r))
                red_buscar(c, &r);
            else
                reply_text(c, 403, "Forbidden", "");
        } else if (get) {
            serve_file(c, &r);
        } else if ((!strcmp(r.method, "PUT") || !strcmp(r.method, "DELETE")) && rel && save_name_ok(rel) && origin_ok(&r)) {
            if (r.method[0] == 'D') {
                char path[300];
                snprintf(path, sizeof path, "save/%.200s", rel); /* (save_name_ok: hasta 200) */
                remove(path);
                reply_text(c, 204, "No Content", "");
            } else if (save_put(c, &r, rel) != 0) {
                break;
            }
        } else {
            int known = !strcmp(r.method, "PUT") || !strcmp(r.method, "DELETE");
            reply_text(c, known ? 403 : 405, known ? "Forbidden" : "Method Not Allowed", "");
            if (!strcmp(r.method, "PUT"))
                break; /* (el cuerpo no se leyó) */
        }
        if (r.close)
            break;
    }
    sock_close(c->fd);
    free(c);
}

/* ---- La prueba (--prueba): lo que lee cosas de afuera ---- */

static int self_test(void)
{
    int bad = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("falló: %s (renglón %d)\n", #cond, __LINE__);        \
            bad++;                                                      \
        }                                                               \
    } while (0)
    char key[32], out[256];
    ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", key); /* (el ejemplo del RFC 6455) */
    CHECK(!strcmp(key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    uint8_t h[20];
    sha1((const uint8_t *)"abc", 3, h);
    base64(h, 20, out);
    CHECK(!strcmp(out, "qZk+NkcGgWq6PiVxeFDCbJzQ2J0="));
    CHECK(safe_path("/data/global/excel/armor.txt", out, sizeof out) && !strcmp(out, "data/global/excel/armor.txt"));
    CHECK(safe_path("/", out, sizeof out) && !out[0]);
    CHECK(safe_path("/a%20b.txt", out, sizeof out) && !strcmp(out, "a b.txt"));
    CHECK(!safe_path("/../secreto", out, sizeof out));
    CHECK(!safe_path("/%2e%2e/secreto", out, sizeof out));
    CHECK(!safe_path("/%2Fetc/passwd", out, sizeof out));
    CHECK(!safe_path("/c:/windows", out, sizeof out));
    CHECK(!safe_path("/a\\b", out, sizeof out));
    CHECK(!safe_path("/a%zz", out, sizeof out));
    CHECK(save_name_ok("slot1.sav") && save_name_ok("cine/visto.txt"));
    CHECK(!save_name_ok("../x") && !save_name_ok("/x") && !save_name_ok("a b") && !save_name_ok(""));
    Req r;
    const char *req = "GET /red/conectar?a=192.168.1.5&p=6112 HTTP/1.1\r\nHost: localhost:8642\r\n"
                      "upgrade: websocket\r\nSec-WebSocket-Key:  abc== \r\nOrigin: http://localhost:8642\r\n"
                      "Content-Length: 12\r\nConnection: Upgrade\r\n";
    s_port = 8642;
    CHECK(parse_req(req, strlen(req), &r) == 1);
    CHECK(!strcmp(r.method, "GET") && !strcmp(r.path, "/red/conectar") && !strcmp(r.query, "a=192.168.1.5&p=6112"));
    CHECK(!strcmp(r.host, "localhost:8642") && !strcmp(r.upgrade, "websocket") && !strcmp(r.wskey, "abc=="));
    CHECK(r.length == 12 && !r.close && origin_ok(&r) && is_me(r.host, "") && port_of(&r) == 6112);
    CHECK(!strcmp(query_get(r.query, "a", out, sizeof out), "192.168.1.5"));
    CHECK(!is_me("evil.com:8642", "") && !is_me("localhost:8643", "") && is_me("127.0.0.1:8642", ""));
    snprintf(r.origin, sizeof r.origin, "http://evil.com");
    CHECK(!origin_ok(&r));
    CHECK(parse_req("GET nada\r\n", 10, &r) < 0 && parse_req("GET /x HTTP/1.0\r\n", 17, &r) == 1 && r.close);
    /* Un mensaje del navegador con máscara, partido en dos pedazos, con un ping en el medio */
    Ws w;
    memset(&w, 0, sizeof w);
    w.fd = INVALID_SOCKET;
    const uint8_t fr[] = { 0x02, 0x82, 1, 2, 3, 4, 'h' ^ 1, 'o' ^ 2, 0x89, 0x80, 0, 0, 0, 0,
                           0x80, 0x82, 9, 9, 9, 9, 'l' ^ 9, 'a' ^ 9, 0x88, 0x80, 0, 0, 0, 0 };
    int op = 0;
    grow(&w.in, &w.in_cap, sizeof fr);
    memcpy(w.in, fr, sizeof fr);
    w.in_len = 6; /* (primero llega solo la mitad del primer pedazo) */
    CHECK(ws_next(&w, &op) == 0);
    w.in_len = sizeof fr;
    CHECK(ws_next(&w, &op) == 1 && op == 2 && w.msg_len == 4 && !memcmp(w.msg, "hola", 4));
    CHECK(ws_next(&w, &op) == 1 && op == 8 && w.in_len == 0);
    w.in[0] = 0x82;
    w.in[1] = 0x01; /* (sin máscara: no es del navegador) */
    w.in_len = 3;
    CHECK(ws_next(&w, &op) == -1);
    free(w.in);
    free(w.msg);
#undef CHECK
    printf(bad ? "%d fallas\n" : "todo bien\n", bad);
    return bad ? 1 : 0;
}

/* ---- El programa ---- */

static void wait_key(void)
{
#ifdef _WIN32
    printf("%s", T("Enter para salir.\n", "Press Enter to exit.\n"));
    getchar();
#endif
}

int main(int argc, char **argv)
{
    int port = getenv("PORT") ? atoi(getenv("PORT")) : WEB_PORT, open_it = !getenv("D2_NO_BROWSER");
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    s_es = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
    wchar_t exe[MAX_PATH]; /* (en la carpeta del programa, con cualquier nombre: con letras de cualquier idioma) */
    DWORD en = GetModuleFileNameW(NULL, exe, MAX_PATH);
    while (en > 0 && exe[en - 1] != L'\\' && exe[en - 1] != L'/')
        en--;
    if (en > 0) {
        exe[en] = 0;
        SetCurrentDirectoryW(exe);
    }
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); /* (el xdg-open que abre el navegador) */
    const char *lang = getenv("LC_ALL");
    if (!lang || !*lang)
        lang = getenv("LC_MESSAGES");
    if (!lang || !*lang)
        lang = getenv("LANG");
    s_es = lang && !strncmp(lang, "es", 2);
    char exe[4096];
    ssize_t en = readlink("/proc/self/exe", exe, sizeof exe - 1);
    while (en > 0 && exe[en - 1] != '/')
        en--;
    if (en > 0) {
        exe[en] = '\0';
        if (chdir(exe) != 0)
            perror(exe);
    }
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "--puerto") || !strcmp(argv[i], "--port")) && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sin-navegador") || !strcmp(argv[i], "--no-browser"))
            open_it = 0;
        else if (!strcmp(argv[i], "--prueba"))
            return self_test();
        else {
            printf("%s", T("Uso: d2port-servidor [--puerto N] [--sin-navegador]\n",
                           "Usage: d2port-servidor [--port N] [--no-browser]\n"));
            return 2;
        }
    }
    if (!file_info("index.html", NULL) || !file_info("d2port.wasm", NULL)) {
        printf("%s", T("Este programa va en la carpeta de la versión para el navegador (al lado de index.html y\n"
                       "d2port.wasm).\n",
                       "This program goes in the folder of the browser version (next to index.html and d2port.wasm).\n"));
        wait_key();
        return 1;
    }
    sock_t l = INVALID_SOCKET;
    for (int tries = 0; tries < 10 && l == INVALID_SOCKET; tries++, port++) { /* (si está ocupado, el siguiente) */
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        l = socket(AF_INET, SOCK_STREAM, 0);
#ifndef _WIN32 /* (al volver a abrirlo enseguida, el puerto sigue con las conexiones de antes cerrándose) */
        int one = 1;
        if (l != INVALID_SOCKET)
            setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
        if (l != INVALID_SOCKET && (bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 64) != 0)) {
            sock_close(l);
            l = INVALID_SOCKET;
        }
        if (l != INVALID_SOCKET)
            s_port = port;
    }
    if (l == INVALID_SOCKET) {
        printf("%s %d\n", T("No pude abrir el puerto", "Could not open port"), port - 10);
        wait_key();
        return 1;
    }
    char url[64];
    snprintf(url, sizeof url, "http://localhost:%d/", s_port);
    printf("Diablo II (port): %s\n", url);
    printf("%s", T("Las partidas se guardan en la carpeta save/ de acá. Cerrá esta ventana para terminar.\n",
                   "Saved games go to the save/ folder here. Close this window to stop.\n"));
    if (!file_info("data/global/animdata.d2", NULL))
        printf("%s", T("Faltan los archivos del juego en data/: importalos primero con el importador de esta carpeta.\n",
                       "The game files are missing from data/: import them first with the importer in this folder.\n"));
    if (open_it)
        open_browser(url);
    for (;;) {
        sock_t s = accept(l, NULL, NULL);
        if (s == INVALID_SOCKET)
            continue;
        Conn *c = calloc(1, sizeof *c);
        if (!c) {
            sock_close(s);
            continue;
        }
        c->fd = s;
        set_nodelay(s);
        if (go(serve, c) != 0) {
            sock_close(s);
            free(c);
        }
    }
}
