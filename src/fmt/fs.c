#include "fs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char s_root[512] = ".";

/* D2_FS_LOG=archivo: anota cada archivo del juego que se lee (para la lista de precarga del navegador) */
static void fs_log(const char *path)
{
    static FILE *f;
    static int on = -1;
    if (on < 0) {
        const char *name = getenv("D2_FS_LOG");
        on = name && (f = fopen(name, "a")) != NULL;
    }
    if (on) {
        fprintf(f, "%s\n", path);
        fflush(f);
    }
}

#ifdef __EMSCRIPTEN__
/* ---- En el navegador (docs/web.md) ----
   Los archivos del juego están en el servidor, en la carpeta data/ al lado de la página (los copia ahí el
   importador). Cada uno se pide cuando hace falta (emscripten_wget_data: con ASYNCIFY el juego espera sin trabar
   la página) y la lista del importador (archivos.txt, al lado de la página) dice cuáles hay sin pedirlos: lo que
   no está en la lista no existe. Lo que el juego escribe (las partidas) está en el sistema de archivos de
   Emscripten (/save, guardado en el navegador): eso se lee de ahí. */
#define WEB_SLOTS 65536 /* (más del doble de los ~34.000 de la lista) */
static char **s_list;   /* la lista, en una tabla de dispersión (rutas en minúsculas, "data/global/...") */
static char **s_order;  /* y en su orden (para fs_find) */
static int    s_nlist = -1;

static unsigned web_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void web_list_load(void)
{
    if (s_nlist >= 0)
        return;
    s_nlist = 0;
    s_list = calloc(WEB_SLOTS, sizeof *s_list);
    void *buf = NULL;
    int size = 0, err = 0;
    emscripten_wget_data("archivos.txt", &buf, &size, &err);
    if (!buf || err || !s_list) {
        fprintf(stderr, "web: no está archivos.txt al lado de la página: no hay archivos del juego\n");
        free(buf);
        return;
    }
    char *text = realloc(buf, (size_t)size + 1);
    text[size] = '\0';
    int lines = 1;
    for (int i = 0; i < size; i++)
        lines += text[i] == '\n';
    s_order = calloc((size_t)lines, sizeof *s_order);
    for (char *line = text, *next; line; line = next) { /* (los renglones vacíos se saltean abajo) */
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = '\0';
        if (!n || line[0] == '#')
            continue;
        for (char *c = line; *c; c++)
            *c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);
        unsigned h = web_hash(line) & (WEB_SLOTS - 1);
        while (s_list[h] && strcmp(s_list[h], line) != 0)
            h = (h + 1) & (WEB_SLOTS - 1);
        if (!s_list[h]) {
            s_list[h] = line; /* (apunta adentro de `text`, que queda para siempre) */
            s_order[s_nlist++] = line;
        }
    }
    printf("web: %d archivos del juego en la lista\n", s_nlist);
}

/* La ruta relativa a la página ("data/global/..."), en minúsculas y con '/' */
static void web_key(const char *path, char *out, size_t cap)
{
    size_t n = 0;
    if (strcmp(s_root, ".") != 0) {
        int k = snprintf(out, cap, "%s/", s_root);
        n = k > 0 && (size_t)k < cap ? (size_t)k : 0;
    }
    for (const char *p = path; *p && n + 1 < cap; p++)
        out[n++] = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
    out[n] = '\0';
}

/* ¿Está en el servidor? (HEAD, sin bajarlo; la respuesta queda para la próxima) */
EM_ASYNC_JS(int, web_head, (const char *key), {
    var k = UTF8ToString(key);
    Module.d2heads = Module.d2heads || new Map();
    if (Module.d2heads.has(k))
        return Module.d2heads.get(k);
    var ok = 0;
    try { ok = (await fetch(k, { method: 'HEAD' })).ok ? 1 : 0; } catch (e) {}
    Module.d2heads.set(k, ok);
    return ok;
});

/* ¿Hay que pedirlo? (está en la lista; o es de lo que pone el importador sin estar en ella: el idioma y las
   cinemáticas, que convierte con ffmpeg; esos se preguntan al servidor) */
static int web_listed(const char *key)
{
    web_list_load();
    if (!strcmp(key, "idioma.txt") || !strncmp(key, "data/global/video/", 18))
        return web_head(key);
    if (s_nlist <= 0)
        return 0;
    unsigned h = web_hash(key) & (WEB_SLOTS - 1);
    while (s_list[h]) {
        if (!strcmp(s_list[h], key))
            return 1;
        h = (h + 1) & (WEB_SLOTS - 1);
    }
    return 0;
}

/* La caché de la página (web/cache.js, adentro de d2port.js): lo precargado en paralelo se toma al instante; lo
   que falta se pide y se espera (ASYNCIFY) */
EM_JS(int, web_cached, (const char *key), {
    var u = Module.d2cache && Module.d2cache.get(UTF8ToString(key));
    return u ? u.length : -1;
});
EM_ASYNC_JS(int, web_fetch, (const char *key), {
    var u = await Module.d2get(UTF8ToString(key));
    return u ? u.length : -1;
});
EM_JS(void, web_take, (const char *key, uint8_t *dst), {
    var u = Module.d2take(UTF8ToString(key));
    if (u) HEAPU8.set(u, dst);
});
EM_JS(void, web_prefetch_dir, (const char *dir), {
    if (Module.d2prefetchDir) Module.d2prefetchDir(UTF8ToString(dir));
});

/* Todo el archivo (del servidor, por la caché): NULL si no está */
static uint8_t *web_read(const char *key, size_t *size)
{
    int n = web_cached(key);
    if (n < 0)
        n = web_fetch(key);
    if (n < 0)
        return NULL;
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf)
        return NULL;
    web_take(key, buf);
    buf[n] = 0;
    if (size)
        *size = (size_t)n;
    return buf;
}

static int web_local(const char *key) /* ¿lo escribió el juego? (en el sistema de archivos de Emscripten) */
{
    struct stat st;
    return stat(key, &st) == 0 && S_ISREG(st.st_mode);
}
#endif

void fs_set_root(const char *root)
{
    snprintf(s_root, sizeof s_root, "%s", root);
}

int fs_find(const char *dir, const char *suffix, char *out, size_t cap)
{
#ifdef __EMSCRIPTEN__
    char key[1024];
    web_key(dir, key, sizeof key);
    web_list_load();
    size_t kl = strlen(key), wl = strlen(suffix);
    for (int i = 0; i < s_nlist; i++) {
        const char *e = s_order[i];
        size_t el = strlen(e);
        if (el > kl + 1 && !strncmp(e, key, kl) && e[kl] == '/' && !strchr(e + kl + 1, '/') && el >= wl &&
            !strcmp(e + el - wl, suffix)) {
            snprintf(out, cap, "%s/%s", dir, e + kl + 1);
            return 0;
        }
    }
    return -1;
#endif
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", s_root, dir);
    DIR *d = opendir(full);
    if (!d)
        return -1;
    size_t sl = strlen(suffix);
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl >= sl && strcmp(e->d_name + nl - sl, suffix) == 0) {
            snprintf(out, cap, "%s/%s", dir, e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

void fs_prefetch_dir(const char *dir)
{
#ifdef __EMSCRIPTEN__
    char key[1024];
    web_key(dir, key, sizeof key);
    web_prefetch_dir(key); /* (en el navegador: se baja en segundo plano a la caché de la página) */
    return;
#endif
#ifdef __linux__
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", s_root, dir);
    DIR *d = opendir(full);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char sub[1024], path[1600];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        snprintf(path, sizeof path, "%s/%s", s_root, sub);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            fs_prefetch_dir(sub);
            continue;
        }
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        close(fd);
    }
    closedir(d);
#else
    (void)dir;
#endif
}

int fs_exists(const char *path)
{
#ifdef __EMSCRIPTEN__
    char key[1024];
    web_key(path, key, sizeof key);
    return web_local(key) || web_listed(key);
#else
    FILE *f = fs_open(path);
    if (f)
        fclose(f);
    return f != NULL;
#endif
}

FILE *fs_open(const char *path)
{
    fs_log(path);
#ifdef __EMSCRIPTEN__
    /* (para leer de a pedazos, la música y las cinemáticas: se baja entero a la memoria, se abre y se borra el
       nombre: queda mientras esté abierto) */
    char key[1024];
    web_key(path, key, sizeof key);
    if (web_local(key))
        return fopen(key, "rb");
    if (!web_listed(key))
        return NULL;
    size_t len = 0;
    uint8_t *buf = web_read(key, &len);
    if (!buf)
        return NULL;
    static int tmp_n;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/tmp/fs%d", tmp_n++);
    FILE *w = fopen(tmp, "wb");
    size_t put = w ? fwrite(buf, 1, len, w) : 0;
    if (w)
        fclose(w);
    free(buf);
    FILE *f = put == len ? fopen(tmp, "rb") : NULL;
    unlink(tmp);
    return f;
#endif
    char full[1024];
    int n = snprintf(full, sizeof full, "%s/", s_root);
    for (const char *p = path; *p && n + 1 < (int)sizeof full; p++)
        full[n++] = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
    full[n] = '\0';
    return fopen(full, "rb");
}

uint8_t *fs_read(const char *path, size_t *size)
{
#ifdef __EMSCRIPTEN__
    char key[1024];
    web_key(path, key, sizeof key);
    if (!web_local(key)) {
        if (!web_listed(key))
            return NULL;
        fs_log(path); /* (lo de la carpeta del juego; lo local lo anota fs_open) */
        return web_read(key, size);
    }
#endif
    FILE *f = fs_open(path);
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf && size)
        *size = (size_t)len;
    return buf;
}
