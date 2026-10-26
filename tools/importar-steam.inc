/* ---- Desde Steam (--steam; docs/steam.md) ----
   Para quien compró Diablo II: Resurrected en Steam (la Infernal Edition, app 2536520) y no lo tiene instalado: el
   importador lo baja de Steam con la cuenta de la persona y después lo importa como siempre. Lo baja DepotDownloader
   (github.com/SteamRE/DepotDownloader, GPL-2.0): no viene en el paquete, se baja la primera vez de su página de
   versiones, siempre la misma (DD_TAG) y revisando su SHA-256. Steam solo entrega la clave de los archivos a una
   cuenta que tiene el juego. Se inicia sesión con un código QR que se escanea con la app de Steam del celular (el
   importador nunca ve la contraseña), o con --usuario (DepotDownloader pregunta la contraseña y el código de Steam
   Guard en la consola).

   Primero baja solo las listas de archivos de Steam (-manifest-only) para saber cuánto pesa y si entra; después lo
   que necesita CascLib (.build.info y Data/), en la misma carpeta (si se corta, la próxima vez sigue donde quedó);
   importa; anota en importar-steam.log de qué archivos data.### salió lo importado (para ver si conviene bajar
   menos) y borra la descarga.

   --ui: para la pantalla de la Pi (src/steamui.c), que lee esta salida: líneas "@@fase texto", "@@progreso N" y
   "@@fin código" además de los mensajes; el código QR de DepotDownloader pasa tal cual. D2_DD=programa (pruebas):
   otro DepotDownloader. */
#include "zlib/zlib.h"

#include <stdint.h>
#ifdef _WIN32
#else
#include <sys/statvfs.h>
#include <sys/wait.h>
#endif

#define STEAM_APP "2536520" /* Diablo II: Resurrected – Infernal Edition */
#define DD_TAG    "DepotDownloader_3.4.0"
#if defined(_WIN32)
#define DD_ZIP "DepotDownloader-windows-x64.zip"
#define DD_SHA "41c9e9f0df54b3ad02e67a11726756e5c73283bd7c2e1b04acfa5ae4c2ed3767"
#define DD_EXE "DepotDownloader.exe"
#elif defined(__aarch64__)
#define DD_ZIP "DepotDownloader-linux-arm64.zip"
#define DD_SHA "d9fb612ccebc1db8eeea3b4045d2221ec70431381393ce908fb72f01d4f9c812"
#define DD_EXE "DepotDownloader"
#elif defined(__x86_64__)
#define DD_ZIP "DepotDownloader-linux-x64.zip"
#define DD_SHA "a999dec66b4850fc961bd50366696d23c2d0fad7b18790e6a5647b2f19097a53"
#define DD_EXE "DepotDownloader"
#else
#define DD_ZIP NULL /* (no hay DepotDownloader para este procesador) */
#define DD_SHA ""
#define DD_EXE "DepotDownloader"
#endif

static int s_ui; /* --ui: la salida la lee la pantalla de la Pi */

static void ui_phase(const char *text)
{
    if (s_ui)
        printf("@@fase %s\n", text);
    else
        printf("\n== %s ==\n", text);
    fflush(stdout);
}

static void ui_progress(double pct)
{
    if (s_ui) {
        printf("@@progreso %.1f\n", pct);
        fflush(stdout);
    }
}

/* Los idiomas de Steam de cada uno de los nuestros (k_langs) */
static const char *steam_language(const char *code)
{
    static const char *const k[][2] = {
        { "esMX", "latam" },   { "enUS", "english" },  { "esES", "spanish" },   { "deDE", "german" },
        { "frFR", "french" },  { "itIT", "italian" },  { "ptBR", "brazilian" }, { "plPL", "polish" },
        { "ruRU", "russian" }, { "jaJP", "japanese" }, { "koKR", "koreana" },   { "zhCN", "schinese" },
        { "zhTW", "tchinese" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(code, k[i][0]))
            return k[i][1];
    return "english";
}

/* ---- SHA-256 (para revisar el DepotDownloader bajado) ---- */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   n;
} Sha256;

static uint32_t ror32(uint32_t v, int n) { return v >> n | v << (32 - n); }

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

static void sha256_init(Sha256 *s)
{
    static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, h0, sizeof h0);
    s->len = 0;
    s->n = 0;
}

static void sha256_update(Sha256 *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n > 0) {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, k);
        s->n += k;
        p += k;
        n -= k;
        if (s->n == 64) {
            sha256_block(s->h, s->buf);
            s->n = 0;
        }
    }
}

static void sha256_hex(Sha256 *s, char out[65])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    sha256_update(s, &pad, 1);
    while (s->n != 56)
        sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++)
        snprintf(out + 8 * i, 9, "%08x", s->h[i]);
}

static int sha256_file(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    Sha256 s;
    sha256_init(&s);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sha256_update(&s, buf, n);
    fclose(f);
    sha256_hex(&s, out);
    return 0;
}

/* ---- Sacar un archivo de un .zip (el de DepotDownloader: sin zip64, guardado o deflate) ---- */

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

static int zip_extract(const char *zip, const char *name, const char *out_path)
{
    FILE *f = fopen(zip, "rb");
    if (!f)
        return -1;
    int ok = 0;
    uint8_t *tail = NULL, *cd = NULL, *in = NULL, *out = NULL;
    FILE *o = NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long tn = size < 65557 ? size : 65557; /* (el final del directorio: 22 bytes y hasta 64 KB de comentario) */
    tail = malloc((size_t)tn);
    if (!tail || fseek(f, size - tn, SEEK_SET) != 0 || fread(tail, 1, (size_t)tn, f) != (size_t)tn)
        goto done;
    long eocd = -1;
    for (long i = tn - 22; i >= 0 && eocd < 0; i--)
        if (le32(tail + i) == 0x06054b50)
            eocd = i;
    if (eocd < 0)
        goto done;
    uint32_t cd_size = le32(tail + eocd + 12), cd_off = le32(tail + eocd + 16), entries = le16(tail + eocd + 10);
    cd = malloc(cd_size ? cd_size : 1);
    if (!cd || cd_off + (uint64_t)cd_size > (uint64_t)size || fseek(f, (long)cd_off, SEEK_SET) != 0 ||
        fread(cd, 1, cd_size, f) != cd_size)
        goto done;
    for (uint32_t e = 0, pos = 0; e < entries && pos + 46 <= cd_size; e++) {
        const uint8_t *h = cd + pos;
        if (le32(h) != 0x02014b50)
            break;
        uint32_t method = le16(h + 10), crc = le32(h + 16), csize = le32(h + 20), usize = le32(h + 24);
        uint32_t nlen = le16(h + 28), xlen = le16(h + 30), clen = le16(h + 32), loff = le32(h + 42);
        if (pos + 46 + nlen > cd_size)
            break;
        if (nlen == strlen(name) && !memcmp(h + 46, name, nlen)) {
            uint8_t lh[30];
            if (fseek(f, (long)loff, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034b50 ||
                fseek(f, (long)(loff + 30 + le16(lh + 26) + le16(lh + 28)), SEEK_SET) != 0 || (method != 0 && method != 8))
                goto done;
            char tmp[1100];
            snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
            o = fopen(tmp, "wb");
            in = malloc(65536);
            out = malloc(65536);
            if (!o || !in || !out)
                goto done;
            z_stream zs;
            memset(&zs, 0, sizeof zs);
            if (method == 8 && inflateInit2(&zs, -15) != Z_OK)
                goto done;
            uLong got_crc = crc32(0L, Z_NULL, 0);
            uint64_t left = csize, written = 0;
            int zr = Z_OK, bad = 0;
            while (!bad && (left > 0 || (method == 8 && zr != Z_STREAM_END))) {
                size_t want = left < 65536 ? (size_t)left : 65536;
                size_t n = want ? fread(in, 1, want, f) : 0;
                if (n != want) {
                    bad = 1;
                    break;
                }
                left -= n;
                if (method == 0) {
                    bad = fwrite(in, 1, n, o) != n;
                    got_crc = crc32(got_crc, in, (uInt)n);
                    written += n;
                    continue;
                }
                zs.next_in = in;
                zs.avail_in = (uInt)n;
                do {
                    zs.next_out = out;
                    zs.avail_out = 65536;
                    zr = inflate(&zs, Z_NO_FLUSH);
                    if (zr != Z_OK && zr != Z_STREAM_END && zr != Z_BUF_ERROR) {
                        bad = 1;
                        break;
                    }
                    size_t have = 65536 - zs.avail_out;
                    bad = fwrite(out, 1, have, o) != have;
                    got_crc = crc32(got_crc, out, (uInt)have);
                    written += have;
                } while (!bad && zs.avail_out == 0);
                if (n == 0 && zr != Z_STREAM_END)
                    bad = 1;
            }
            if (method == 8)
                inflateEnd(&zs);
            ok = !bad && written == usize && got_crc == crc && fclose(o) == 0;
            o = NULL;
            if (ok) {
                remove(out_path);
                ok = rename(tmp, out_path) == 0;
            }
            if (!ok)
                remove(tmp);
            goto done;
        }
        pos += 46 + nlen + xlen + clen;
    }
done:
    if (o)
        fclose(o);
    fclose(f);
    free(tail);
    free(cd);
    free(in);
    free(out);
    return ok ? 0 : -1;
}

/* ---- Correr un programa con su salida por acá ---- */

typedef struct {
    char account[128]; /* la cuenta, para la segunda vez sin el QR (la escribe DepotDownloader al iniciar sesión) */
    int  failed;       /* dijo que algo no se pudo */
    double last_pct;
} RunState;

/* Cada renglón de DepotDownloader: lo que importa se anota; con --ui, el avance pasa como "@@progreso" (en la
   consola sale tal cual) y no se repiten los miles de renglones de cada archivo */
static void dd_line(char *line, RunState *st)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';
    const char *acc = strstr(line, "login with -username ");
    if (acc) {
        acc += strlen("login with -username ");
        size_t k = strcspn(acc, " ");
        if (k > 0 && k < sizeof st->account) {
            memcpy(st->account, acc, k);
            st->account[k] = '\0';
        }
    }
    if (strstr(line, "Unable to") || strstr(line, "not available from this account") || strstr(line, "was not completely") ||
        strstr(line, "Aborting") || strstr(line, "No valid depot key"))
        st->failed = 1;
    char *pct = strchr(line, '%');
    double p = 0;
    int is_pct = pct && pct - line <= 8 && sscanf(line, "%lf%%", &p) == 1;
    if (!s_ui)
        return; /* (en la consola ya salió tal cual) */
    if (is_pct) {
        if (p - st->last_pct >= 0.5 || p >= 100.0) {
            st->last_pct = p;
            ui_progress(p);
        }
        return;
    }
    if (!strncmp(line, "Pre-allocating", 14) || !strncmp(line, "Validating", 10) || !strncmp(line, "Deleted", 7))
        return;
    printf("%s\n", line);
    fflush(stdout);
}

#ifdef _WIN32
/* Un argumento para la línea de comandos de Windows (entre comillas, con las barras antes de una comilla dobladas) */
static void win_quote(char *out, size_t cap, const char *arg)
{
    size_t n = strlen(out);
    if (n + 3 >= cap)
        return;
    out[n++] = '"';
    for (const char *p = arg;; p++) {
        size_t bs = 0;
        while (*p == '\\')
            p++, bs++;
        size_t need = *p == '\0' ? bs * 2 : *p == '"' ? bs * 2 + 1 : bs;
        if (n + need + 3 >= cap)
            break;
        for (size_t i = 0; i < need; i++)
            out[n++] = '\\';
        if (*p == '\0')
            break;
        out[n++] = *p;
    }
    out[n++] = '"';
    out[n++] = ' ';
    out[n] = '\0';
}
#endif

/* argv[0]: con la ruta, o el nombre de un programa del sistema (curl). Devuelve su código de salida, -1 si no arrancó.
   echo: la salida pasa a la pantalla tal cual, en cuanto llega (las preguntas de la contraseña no terminan el
   renglón); si no, solo por dd_line */
static int run_program(char *const argv[], RunState *st, int echo)
{
    char line[4096];
    size_t ln = 0;
#ifdef _WIN32
    char cmd[8192] = "";
    for (int i = 0; argv[i]; i++)
        win_quote(cmd, sizeof cmd, argv[i]);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = si.hStdError = wr;
    SetEnvironmentVariableA("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1");
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    char buf[4096];
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got > 0) {
#else
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        setenv("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", 1); /* (sin ICU, que Batocera no trae) */
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t got;
    while ((got = read(fds[0], buf, sizeof buf)) > 0) {
#endif
        if (echo) {
            fwrite(buf, 1, (size_t)got, stdout);
            fflush(stdout);
        }
        for (size_t i = 0; i < (size_t)got; i++) {
            if (buf[i] == '\n' || ln + 1 >= sizeof line) {
                line[ln] = '\0';
                if (st)
                    dd_line(line, st);
                ln = 0;
            } else {
                line[ln++] = buf[i];
            }
        }
    }
    if (ln && st) {
        line[ln] = '\0';
        dd_line(line, st);
    }
#ifdef _WIN32
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* ---- Lugar en el disco y carpetas ---- */

static long long free_bytes(const char *dir)
{
#ifdef _WIN32
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExA(dir, &avail, NULL, NULL))
        return (long long)avail.QuadPart;
#else
    struct statvfs s;
    if (statvfs(dir, &s) == 0)
        return (long long)s.f_bavail * (long long)s.f_frsize;
#endif
    return -1;
}

/* Recorre una carpeta: fn con cada archivo (sin los "." y ".."); depth hasta 8 niveles */
typedef void (*WalkFn)(const char *path, int is_dir, long long size, void *ctx);
static void walk(const char *dir, WalkFn fn, void *ctx, int depth)
{
    if (depth > 8)
        return;
#ifdef _WIN32
    char pat[1100];
    join(pat, sizeof pat, dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        char p[1100];
        join(p, sizeof p, dir, fd.cFileName);
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_dir)
            walk(p, fn, ctx, depth + 1);
        fn(p, is_dir, (long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1100];
        join(p, sizeof p, dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0)
            continue;
        int is_dir = S_ISDIR(st.st_mode);
        if (is_dir)
            walk(p, fn, ctx, depth + 1);
        fn(p, is_dir, (long long)st.st_size, ctx);
    }
    closedir(d);
#endif
}

static void add_size(const char *path, int is_dir, long long size, void *ctx)
{
    (void)path;
    if (!is_dir)
        *(long long *)ctx += size;
}

static void remove_one(const char *path, int is_dir, long long size, void *ctx)
{
    (void)size;
    (void)ctx;
#ifdef _WIN32
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (is_dir)
        RemoveDirectoryA(path);
    else
        DeleteFileA(path);
#else
    if (is_dir)
        rmdir(path);
    else
        unlink(path);
#endif
}

static void remove_tree(const char *dir)
{
    walk(dir, remove_one, NULL, 0);
    remove_one(dir, 1, 0, NULL);
}

static void remove_manifest(const char *path, int is_dir, long long size, void *ctx);

/* Borra solo lo que bajó Steam (Data/, .build.info, lo de DepotDownloader y sus listas), nunca la carpeta entera:
   con --descarga puede ser una carpeta de la persona */
static void remove_download(const char *dl)
{
    char p[1100];
    join(p, sizeof p, dl, "Data");
    remove_tree(p);
    join(p, sizeof p, dl, ".DepotDownloader");
    remove_tree(p);
    join(p, sizeof p, dl, ".build.info");
    remove(p);
    walk(dl, remove_manifest, NULL, 0);
    remove_one(dl, 1, 0, NULL); /* (si quedó vacía) */
}

/* ---- Lo que va a bajar: las listas de Steam (manifest_<depot>_<id>.txt de -manifest-only) ---- */

#define MAX_DEPOTS 64
typedef struct {
    unsigned  id;
    long long bytes;
    int       files, casc; /* casc: trae .build.info o algo de Data/ (lo que lee CascLib) */
} Depot;

typedef struct {
    Depot n[MAX_DEPOTS];
    int   count;
} Depots;

static void read_manifest(const char *path, Depots *ds)
{
    FILE *f = fopen(path, "r");
    if (!f || ds->count >= MAX_DEPOTS) {
        if (f)
            fclose(f);
        return;
    }
    Depot *d = &ds->n[ds->count];
    memset(d, 0, sizeof *d);
    char line[2048];
    int body = 0;
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        unsigned id;
        long long v;
        if (sscanf(line, "Content Manifest for Depot %u", &id) == 1)
            d->id = id;
        else if (sscanf(line, "Total bytes on disk : %lld", &v) == 1) /* (el espacio antes de ":" toma los que haya) */
            d->bytes = v;
        else if (strstr(line, "Size Chunks File SHA"))
            body = 1;
        else if (body) {
            long long size;
            int chunks, used = 0;
            char sha[64];
            unsigned flags;
            if (sscanf(line, "%lld %d %63s %x %n", &size, &chunks, sha, &flags, &used) == 4 && used > 0) {
                char name[1024];
                normalize(line + used, name, sizeof name); /* (minúsculas, con /) */
                d->files++;
                if (!strcmp(name, ".build.info") || !strncmp(name, "data/", 5))
                    d->casc = 1;
            }
        }
    }
    fclose(f);
    if (d->id)
        ds->count++;
}

static void find_manifest(const char *path, int is_dir, long long size, void *ctx)
{
    (void)size;
    const char *base = strrchr(path, '/');
    const char *b2 = strrchr(path, '\\');
    if (b2 > base)
        base = b2;
    base = base ? base + 1 : path;
    if (!is_dir && !strncmp(base, "manifest_", 9) && strstr(base, ".txt"))
        read_manifest(path, ctx);
}

static void remove_manifest(const char *path, int is_dir, long long size, void *ctx)
{
    (void)size;
    (void)ctx;
    const char *base = strrchr(path, '/');
    const char *b2 = strrchr(path, '\\');
    if (b2 > base)
        base = b2;
    base = base ? base + 1 : path;
    if (!is_dir && !strncmp(base, "manifest_", 9))
        remove(path);
}

/* ---- De qué data.### salió cada archivo importado (para ver cuánto de lo bajado se usó) ---- */

#define MAX_SEGS 1024
static int       s_seg_on;
static long long s_seg_used[MAX_SEGS];
static int       s_seg_files[MAX_SEGS];

static void seg_note(HANDLE file)
{
    CASC_FILE_FULL_INFO fi;
    if (s_seg_on && CascGetFileInfo(file, CascFileFullInfo, &fi, sizeof fi, NULL) && fi.SegmentIndex < MAX_SEGS) {
        s_seg_used[fi.SegmentIndex] += (long long)fi.EncodedSize;
        s_seg_files[fi.SegmentIndex]++;
    }
}

static void seg_report(const char *game, FILE *log)
{
    long long total = 0, used_total = 0, used_arch = 0;
    int archives = 0, used = 0;
    for (int i = 0; i < MAX_SEGS; i++) {
        char name[64], path[1100];
        snprintf(name, sizeof name, "Data/data/data.%03d", i);
        join(path, sizeof path, game, name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        archives++;
        total += (long long)st.st_size;
        if (s_seg_files[i]) {
            used++;
            used_arch += (long long)st.st_size;
            used_total += s_seg_used[i];
        }
        if (log)
            fprintf(log, "data.%03d %10.1f MB: %6d archivos, %8.1f MB usados\n", i, st.st_size / 1048576.0, s_seg_files[i],
                    s_seg_used[i] / 1048576.0);
    }
    char line[256];
    snprintf(line, sizeof line,
             T("Lo importado sale de %d de los %d archivos data.### (%.1f de %.1f GB; lo importado: %.2f GB)",
               "The import came from %d of the %d data.### files (%.1f of %.1f GB; the imported files: %.2f GB)"),
             used, archives, used_arch / 1073741824.0, total / 1073741824.0, used_total / 1073741824.0);
    printf("%s\n", line);
    if (log)
        fprintf(log, "%s\n", line);
}

/* ---- DepotDownloader ---- */

/* Lo deja en <dir>/DD_EXE: si no está, lo baja (curl, o wget) y revisa que sea el de siempre */
static int get_depotdownloader(const char *dir, char *exe, size_t cap)
{
    join(exe, cap, dir, DD_EXE);
    const char *forced = getenv("D2_DD");
    if (forced) { /* (las pruebas: otro programa que hace de DepotDownloader) */
        snprintf(exe, cap, "%s", forced);
        return 0;
    }
    if (exists(exe))
        return 0;
    if (!DD_ZIP) {
        printf("%s", T("No hay DepotDownloader para este procesador.\n", "There is no DepotDownloader for this processor.\n"));
        return -1;
    }
    mkdirs_for(exe);
    char zip[1100], url[256];
    join(zip, sizeof zip, dir, DD_ZIP);
    snprintf(url, sizeof url, "https://github.com/SteamRE/DepotDownloader/releases/download/" DD_TAG "/" DD_ZIP);
    ui_phase(T("Bajando DepotDownloader (unos 32 MB, una sola vez)", "Downloading DepotDownloader (about 32 MB, only once)"));
    printf("%s\n", url);
    fflush(stdout);
    int r;
#ifdef _WIN32
    char curl[MAX_PATH + 16], sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    snprintf(curl, sizeof curl, "%s\\curl.exe", sys); /* (viene con Windows 10 y 11) */
    char *args[] = { curl, "-L", "-f", "-sS", "--retry", "3", "-o", zip, url, NULL };
    r = run_program(args, NULL, 1);
#else
    char *args[] = { "curl", "-L", "-f", "-sS", "--retry", "3", "-o", zip, url, NULL };
    r = run_program(args, NULL, 1);
    if (r == 127 || r < 0) { /* (sin curl: wget, que está en casi todos lados, Batocera incluido) */
        char *wargs[] = { "wget", "-q", "-O", zip, url, NULL };
        r = run_program(wargs, NULL, 1);
    }
#endif
    char sum[65];
    if (r != 0 || sha256_file(zip, sum) != 0) {
        printf(T("No pude bajar DepotDownloader (¿hay internet?). También se puede bajar a mano de\n%s\n"
                 "y dejar %s en %s\n",
                 "Could not download DepotDownloader (is there internet?). You can also download it by hand from\n%s\n"
                 "and put %s in %s\n"),
               url, DD_EXE, dir);
        remove(zip);
        return -1;
    }
    if (strcmp(sum, DD_SHA) != 0) {
        printf(T("El DepotDownloader bajado no es el esperado (SHA-256 %s): no lo uso.\n",
                 "The downloaded DepotDownloader is not the expected one (SHA-256 %s): not using it.\n"),
               sum);
        remove(zip);
        return -1;
    }
    char lic[1100];
    join(lic, sizeof lic, dir, "LICENSE-DepotDownloader.txt");
    if (zip_extract(zip, DD_EXE, exe) != 0) {
        printf(T("No pude sacar %s de %s\n", "Could not extract %s from %s\n"), DD_EXE, zip);
        return -1;
    }
    zip_extract(zip, "LICENSE", lic); /* (GPL-2.0: su licencia, al lado) */
    remove(zip);
#ifndef _WIN32
    chmod(exe, 0755);
#endif
    return 0;
}

/* Otro disco con lugar para la descarga (un USB en /media, como los monta Batocera; o en /run/media/<usuario> o
   /mnt), que no sea el de not_here: la carpeta en out. 0 = encontró */
static int other_disk(long long need, const char *not_here, char *out, size_t cap)
{
#ifdef _WIN32
    (void)need;
    (void)not_here;
    (void)out;
    (void)cap;
    return -1; /* (en Windows: --descarga) */
#else
    struct stat here;
    if (stat(not_here, &here) != 0)
        return -1;
    static const char *const k_roots[] = { "/media", "/run/media", "/mnt" };
    for (size_t i = 0; i < sizeof k_roots / sizeof k_roots[0]; i++) {
        for (int depth = 0; depth < 2; depth++) { /* (/media/USB, o /run/media/<usuario>/USB) */
            DIR *d = opendir(k_roots[i]);
            if (!d)
                break;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char p[1100], q[1100];
                if (e->d_name[0] == '.')
                    continue;
                join(p, sizeof p, k_roots[i], e->d_name);
                DIR *d2 = depth ? opendir(p) : NULL;
                struct dirent *e2 = d2 ? readdir(d2) : NULL;
                do {
                    if (depth && (!e2 || e2->d_name[0] == '.'))
                        continue;
                    if (depth)
                        join(q, sizeof q, p, e2->d_name);
                    else
                        snprintf(q, sizeof q, "%s", p);
                    struct stat st;
                    if (stat(q, &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev != here.st_dev && access(q, W_OK) == 0 &&
                        free_bytes(q) >= need) {
                        join(out, cap, q, "d2port-steam-descarga");
                        if (d2)
                            closedir(d2);
                        closedir(d);
                        return 0;
                    }
                } while (depth && d2 && (e2 = readdir(d2)) != NULL);
                if (d2)
                    closedir(d2);
            }
            closedir(d);
        }
    }
    return -1;
#endif
}

/* Baja el juego de Steam a dl (si se cortó antes, sigue) y deja en game la carpeta para CascLib. Si no entra y la
   carpeta no la eligió la persona (dl_fixed), la cambia por otro disco con lugar. 0 = bien */
static int steam_download(const char *pkg, const char *lang, const char *user, char *dl, size_t dl_cap, int dl_fixed,
                          char *game, size_t cap)
{
    char dd_dir[1100], exe[1100], list[1100];
    join(dd_dir, sizeof dd_dir, pkg, "steam");
    if (get_depotdownloader(dd_dir, exe, sizeof exe) != 0)
        return -1;
    char probe[1100];
    join(probe, sizeof probe, dl, ".");
    mkdirs_for(probe);
    MKDIR(dl);

    printf("%s", T("BETA: importar desde Steam todavía no se probó con una cuenta de Steam de verdad. Si falla, mandá esta "
                   "salida e importar-steam.log a quien te pasó el port; importar desde el juego instalado es lo probado.\n",
                   "BETA: importing from Steam has not been tried yet with a real Steam account. If it fails, send this "
                   "output and importar-steam.log to whoever gave you the port; importing from the installed game is the "
                   "tested way.\n"));

    /* 1. Iniciar sesión y bajar solo las listas */
    ui_phase(T("Iniciá sesión en Steam", "Sign in to Steam"));
    if (!user)
        printf("%s", T("Abrí la app de Steam en el celular, tocá el escudo (Steam Guard) y escaneá el código QR.\n",
                       "Open the Steam app on your phone, tap the shield (Steam Guard) and scan the QR code.\n"));
    fflush(stdout);
    walk(dl, remove_manifest, NULL, 0);
    RunState st;
    memset(&st, 0, sizeof st);
    if (user)
        snprintf(st.account, sizeof st.account, "%s", user);
    char *a1[32];
    int n = 0;
    a1[n++] = exe;
    a1[n++] = "-app";
    a1[n++] = STEAM_APP;
    a1[n++] = "-os";
    a1[n++] = "windows";
    a1[n++] = "-osarch";
    a1[n++] = "64";
    a1[n++] = "-language";
    a1[n++] = (char *)steam_language(lang);
    a1[n++] = "-dir";
    a1[n++] = (char *)dl;
    a1[n++] = "-manifest-only";
    a1[n++] = "-remember-password"; /* (así la descarga no vuelve a pedir el QR) */
    if (user) {
        a1[n++] = "-username";
        a1[n++] = (char *)user;
    } else {
        a1[n++] = "-qr";
    }
    a1[n] = NULL;
    int r = run_program(a1, &st, !s_ui);
    Depots ds;
    memset(&ds, 0, sizeof ds);
    walk(dl, find_manifest, &ds, 0);
    long long need = 0;
    int casc = 0;
    for (int i = 0; i < ds.count; i++) {
        printf(T("  depot %u: %d archivos, %.2f GB%s\n", "  depot %u: %d files, %.2f GB%s\n"), ds.n[i].id, ds.n[i].files,
               ds.n[i].bytes / 1073741824.0, ds.n[i].casc ? "" : T(" (no hace falta)", " (not needed)"));
        if (ds.n[i].casc) {
            need += ds.n[i].bytes;
            casc++;
        }
    }
    if (r != 0 || casc == 0) {
        printf("%s", T("No se pudo: o no se inició la sesión, o esta cuenta de Steam no tiene Diablo II: Resurrected.\n",
                       "It did not work: either the sign-in failed or this Steam account does not own Diablo II: "
                       "Resurrected.\n"));
        return -1;
    }

    /* 2. ¿Entra? (lo que ya se bajó antes cuenta) */
    long long have = 0;
    char sub[1100];
    join(sub, sizeof sub, dl, "Data");
    walk(sub, add_size, &have, 0);
    long long avail = free_bytes(dl), more = need - have > 0 ? need - have : 0;
    printf(T("Hay que bajar %.1f GB (ya hay %.1f GB); libres en el disco: %.1f GB.\n",
             "To download: %.1f GB (%.1f GB already there); free on the disk: %.1f GB.\n"),
           need / 1073741824.0, have / 1073741824.0, avail / 1073741824.0);
    char usb[1024];
    if (avail >= 0 && avail < more + (512LL << 20) && !dl_fixed && have == 0 &&
        other_disk(need + (512LL << 20), dl, usb, sizeof usb) == 0) {
        printf(T("No entra en %s: lo bajo a %s (se borra al terminar).\n",
                 "It does not fit in %s: downloading to %s instead (it is deleted at the end).\n"),
               dl, usb);
        remove_download(dl);
        snprintf(dl, dl_cap, "%s", usb);
        MKDIR(dl);
        avail = free_bytes(dl);
        more = need;
    }
    if (avail >= 0 && avail < more + (512LL << 20)) {
        printf(T("No entra en %s. Liberá lugar, enchufá un disco USB con %.0f GB libres, o usá --descarga <carpeta> en "
                 "otro disco (se borra al terminar).\n",
                 "It does not fit in %s. Free some space, plug in a USB drive with %.0f GB free, or use --descarga "
                 "<folder> on another disk (it is deleted at the end).\n"),
               dl, need / 1073741824.0 + 1);
        return -1;
    }

    /* 3. Bajarlo: solo .build.info y Data/, de los depots que los traen */
    join(list, sizeof list, dd_dir, "lista-steam.txt");
    FILE *lf = fopen(list, "w");
    if (!lf)
        return -1;
    fprintf(lf, ".build.info\nregex:^data/\n");
    fclose(lf);
    char depot_ids[MAX_DEPOTS][16];
    char *a2[32 + 2 * MAX_DEPOTS];
    n = 0;
    a2[n++] = exe;
    a2[n++] = "-app";
    a2[n++] = STEAM_APP;
    a2[n++] = "-os";
    a2[n++] = "windows";
    a2[n++] = "-osarch";
    a2[n++] = "64";
    a2[n++] = "-language";
    a2[n++] = (char *)steam_language(lang);
    a2[n++] = "-dir";
    a2[n++] = (char *)dl;
    a2[n++] = "-filelist";
    a2[n++] = list;
    a2[n++] = "-max-downloads";
    a2[n++] = "8";
    a2[n++] = "-remember-password";
    if (st.account[0]) {
        a2[n++] = "-username";
        a2[n++] = st.account;
    } else {
        a2[n++] = "-qr";
    }
    for (int i = 0; i < ds.count; i++)
        if (ds.n[i].casc) {
            snprintf(depot_ids[i], sizeof depot_ids[i], "%u", ds.n[i].id);
            a2[n++] = "-depot";
            a2[n++] = depot_ids[i];
        }
    a2[n] = NULL;
    char title[160];
    snprintf(title, sizeof title, T("Bajando Diablo II: Resurrected de Steam (%.1f GB)", "Downloading Diablo II: Resurrected from Steam (%.1f GB)"),
             need / 1073741824.0);
    ui_phase(title);
    ui_progress(0);
    st.failed = 0;
    st.last_pct = 0;
    r = run_program(a2, &st, !s_ui);
    snprintf(game, cap, "%s", dl);
    if (r != 0 || st.failed || !is_game_dir(game)) {
        printf("%s", T("La descarga no terminó. Volvé a abrir el importador: sigue donde quedó.\n",
                       "The download did not finish. Open the importer again: it resumes where it stopped.\n"));
        return -1;
    }
    return 0;
}

/* --prueba-steam [zip nombre sha256]: lo que lee cosas de afuera (las listas de Steam, la salida de DepotDownloader) y
   el SHA-256; con un .zip, saca ese archivo y compara su SHA-256 (sin correr nada) */
static int steam_self_test(int argc, char **argv)
{
    int bad = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            printf("falló: %s (renglón %d)\n", #c, __LINE__);     \
            bad++;                                                \
        }                                                         \
    } while (0)
    Sha256 s;
    char hex[65];
    sha256_init(&s);
    sha256_update(&s, (const uint8_t *)"abc", 3);
    sha256_hex(&s, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha256_init(&s);
    sha256_hex(&s, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha256_init(&s);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* (dos bloques) */
    for (size_t i = 0; two[i]; i++)
        sha256_update(&s, (const uint8_t *)two + i, 1);
    sha256_hex(&s, hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK(!strcmp(steam_language("esMX"), "latam") && !strcmp(steam_language("zhTW"), "tchinese"));
    /* Una lista de Steam como las de -manifest-only */
    const char *mf = "prueba-manifest_1_2.txt";
    FILE *f = fopen(mf, "w");
    if (f) {
        fprintf(f, "Content Manifest for Depot 2536523 \n\nManifest ID / date     : 2 / 1/1/2026 \n"
                   "Total number of files  : 3 \nTotal number of chunks : 9 \nTotal bytes on disk    : 14034506970 \n"
                   "Total bytes compressed : 13777802320 \n\n\n"
                   "          Size Chunks File SHA                                 Flags Name\n"
                   "           123      1 0123456789abcdef0123456789abcdef01234567     0 .build.info\n"
                   "    1073741824   1024 0123456789abcdef0123456789abcdef01234567     0 Data\\data\\data.000\n"
                   "             0      0 0000000000000000000000000000000000000000    40 Data\\data\n");
        fclose(f);
    }
    Depots ds;
    memset(&ds, 0, sizeof ds);
    read_manifest(mf, &ds);
    remove(mf);
    CHECK(ds.count == 1 && ds.n[0].id == 2536523 && ds.n[0].bytes == 14034506970LL && ds.n[0].files == 3 && ds.n[0].casc);
    RunState st;
    memset(&st, 0, sizeof st);
    char l1[] = "Success! Next time you can login with -username fulano_99 -remember-password instead of -qr.\r";
    dd_line(l1, &st);
    CHECK(!strcmp(st.account, "fulano_99") && !st.failed);
    char l2[] = "Depot 2536523 is not available from this account.";
    dd_line(l2, &st);
    CHECK(st.failed);
#ifdef _WIN32
    char cmd[256] = "";
    win_quote(cmd, sizeof cmd, "C:\\a b\\x.exe");
    win_quote(cmd, sizeof cmd, "dice \"hola\"\\");
    CHECK(!strcmp(cmd, "\"C:\\a b\\x.exe\" \"dice \\\"hola\\\"\\\\\" "));
#endif
    if (argc >= 3) {
        char sum[65] = "";
        CHECK(zip_extract(argv[0], argv[1], "prueba-zip.out") == 0 && sha256_file("prueba-zip.out", sum) == 0);
        CHECK(!strcmp(sum, argv[2]));
        remove("prueba-zip.out");
        CHECK(zip_extract(argv[0], "no-esta", "prueba-zip.out") != 0 && !exists("prueba-zip.out"));
    }
#undef CHECK
    printf(bad ? "%d fallas\n" : "todo bien\n", bad);
    return bad ? 1 : 0;
}
