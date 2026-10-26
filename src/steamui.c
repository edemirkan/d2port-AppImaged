#include "steamui.h"

#include <stdio.h>

/* La pantalla de "Importar desde Steam" (steamui.h, docs/steam.md). En la Pi el importador corre desde el menú de
   Ports, sin consola a la vista: esta pantalla corre d2port-importar --steam --ui (al lado de este programa), lee su
   salida y muestra el código QR de DepotDownloader para escanear con la app de Steam (los renglones de "██" y "  ",
   dos caracteres por módulo), la fase ("@@fase"), el avance ("@@progreso") y los últimos mensajes; todo pasa también
   a la salida, que el script de Ports guarda en /userdata/system/logs/d2port-steam.log. Dibuja sin los archivos del
   juego, que todavía no están: la letra es font8x8.inc. B (o Esc) cancela (la descarga sigue donde quedó la próxima
   vez); al terminar, A o B sale.
   Pruebas: D2_STEAMUI_SCRIPT=archivo lee esa salida en vez de correr el importador; D2_STEAMUI_SHOT=archivo.bmp dibuja
   eso en una pantalla de mentira (sin ventana), la guarda y sale. */

#if defined(_WIN32) || defined(__EMSCRIPTEN__)

int steamui_run(const char *lang)
{
    (void)lang;
    fprintf(stderr, "--importar-steam es para la Pi: en la PC, d2port-importar --steam en una consola\n");
    return 2;
}

#else

#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "font8x8.inc"

#define W         640 /* la pantalla, en estos píxeles (se agranda a la de verdad) */
#define H         360
#define LOG_LINES 8
#define LOG_BYTES 240 /* (en UTF-8; se muestra hasta lo que entra en el ancho) */
#define QR_MAX    177 /* (el QR más grande, versión 40) */

static int           s_es;
static char          s_phase[160], s_log[LOG_LINES][LOG_BYTES + 1];
static int           s_nlog;
static double        s_progress = -1;
static unsigned char s_qr[QR_MAX][QR_MAX], s_rows[QR_MAX][QR_MAX];
static int           s_qr_n, s_qr_on, s_qr_dirty, s_rows_n, s_row_len;

#define TX(es, en) (s_es ? (es) : (en))

/* ---- Lo que dice el importador ---- */

/* ¿Un renglón del QR? Solo "██" (oscuro) y "  " (claro), dos caracteres por módulo; n = módulos */
static int qr_row(const char *s, unsigned char *mods, int *n)
{
    int k = 0;
    while (*s) {
        if (!strncmp(s, "\xE2\x96\x88\xE2\x96\x88", 6)) {
            mods[k] = 1;
            s += 6;
        } else if (s[0] == ' ' && s[1] == ' ') {
            mods[k] = 0;
            s += 2;
        } else {
            return 0;
        }
        if (++k >= QR_MAX)
            return 0;
    }
    *n = k;
    return k >= 21;
}

/* El bloque que llegó, si es un cuadrado entero, pasa a ser el que se muestra */
static void qr_flush(void)
{
    if (s_rows_n >= 21 && s_rows_n == s_row_len) {
        memcpy(s_qr, s_rows, sizeof s_qr);
        s_qr_n = s_rows_n;
        s_qr_on = s_qr_dirty = 1;
    }
    s_rows_n = 0;
}

static void log_push(const char *line)
{
    char clean[LOG_BYTES + 1];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)line; *p && n < LOG_BYTES; p++)
        if (*p >= 32)
            clean[n++] = (char)*p;
    clean[n] = '\0';
    if (!n)
        return;
    if (s_nlog == LOG_LINES) {
        memmove(s_log[0], s_log[1], sizeof s_log[0] * (LOG_LINES - 1));
        s_nlog--;
    }
    snprintf(s_log[s_nlog++], sizeof s_log[0], "%s", clean);
}

static void on_line(char *line)
{
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n'))
        line[--len] = '\0';
    unsigned char mods[QR_MAX];
    int n = 0;
    if (len && qr_row(line, mods, &n)) {
        if (s_rows_n && n != s_row_len)
            qr_flush();
        memcpy(s_rows[s_rows_n], mods, (size_t)n);
        s_row_len = n;
        if (++s_rows_n == s_row_len)
            qr_flush();
        return;
    }
    if (s_rows_n)
        qr_flush();
    printf("%s\n", line); /* (al registro) */
    fflush(stdout);
    if (!strncmp(line, "@@fase ", 7)) {
        snprintf(s_phase, sizeof s_phase, "%s", line + 7);
        s_progress = -1;
        s_qr_on = 0; /* (otra fase: ya se inició la sesión) */
    } else if (!strncmp(line, "@@progreso ", 11)) {
        s_progress = atof(line + 11);
    } else {
        if (strstr(line, "Success!") || strstr(line, "into Steam3... Done"))
            s_qr_on = 0; /* (se inició la sesión) */
        log_push(line);
    }
}

/* ---- Dibujar ---- */

static SDL_Renderer *s_r;
static SDL_Texture  *s_font, *s_qr_tex;

static int font_make(void)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 128, 128, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return -1;
    SDL_FillRect(s, NULL, 0);
    for (int c = 0; c < 256; c++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (k_font8[c][y] >> x & 1)
                    ((Uint32 *)s->pixels)[((c / 16) * 8 + y) * (s->pitch / 4) + (c % 16) * 8 + x] = 0xFFFFFFFF;
    s_font = SDL_CreateTextureFromSurface(s_r, s);
    SDL_FreeSurface(s);
    if (!s_font)
        return -1;
    SDL_SetTextureBlendMode(s_font, SDL_BLENDMODE_BLEND);
    return 0;
}

/* El próximo carácter de un texto UTF-8 (lo que no es Latin-1, "?") */
static int utf8_next(const unsigned char **p)
{
    const unsigned char *s = *p;
    int c = *s++;
    if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80)
        c = (c & 0x1F) << 6 | (*s++ & 0x3F);
    else if (c >= 0x80) {
        while ((*s & 0xC0) == 0x80)
            s++;
        c = '?';
    }
    *p = s;
    return c < 256 ? c : '?';
}

static void text(int x, int y, int scale, Uint32 rgb, const char *str, int max_chars)
{
    SDL_SetTextureColorMod(s_font, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
    const unsigned char *p = (const unsigned char *)str;
    for (int i = 0; *p && i < max_chars; i++) {
        int c = utf8_next(&p);
        SDL_Rect src = { (c % 16) * 8, (c / 16) * 8, 8, 8 }, dst = { x + i * 8 * scale, y, 8 * scale, 8 * scale };
        SDL_RenderCopy(s_r, s_font, &src, &dst);
    }
}

/* Un texto largo cortado por palabras en renglones de width caracteres; devuelve la y de abajo */
static int text_wrap(int x, int y, Uint32 rgb, const char *str, int width, int line_h)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", str);
    char *s = buf;
    while (*s) {
        int n = 0, last_space = -1, chars = 0;
        const unsigned char *p = (const unsigned char *)s;
        while (*p && chars < width) {
            if (*p == ' ')
                last_space = (int)((const char *)p - s);
            utf8_next(&p);
            chars++;
        }
        n = *p && last_space > 0 ? last_space : (int)((const char *)p - s);
        char keep = s[n];
        s[n] = '\0';
        text(x, y, 1, rgb, s, width);
        s[n] = keep;
        s += n;
        while (*s == ' ')
            s++;
        y += line_h;
    }
    return y;
}

static void fill(int x, int y, int w, int h, Uint32 rgb)
{
    SDL_SetRenderDrawColor(s_r, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, 255);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(s_r, &rc);
}

static void draw(int done, int code, int cancelled)
{
    fill(0, 0, W, H, 0x0A0806);
    text(16, 12, 2, 0xC8A050, TX("IMPORTAR DESDE STEAM (BETA)", "IMPORT FROM STEAM (BETA)"), 40);
    const char *phase = s_phase;
    if (done)
        phase = cancelled ? TX("Cancelado: la próxima vez sigue donde quedó.", "Cancelled: next time it resumes where it stopped.")
                : code == 0 ? TX("Listo: ya podés jugar a Diablo II desde el menú de Ports.",
                                 "Done: you can play Diablo II from the Ports menu.")
                            : TX("No se pudo. El registro: /userdata/system/logs/d2port-steam.log",
                                 "It did not work. The log: /userdata/system/logs/d2port-steam.log");
    text_wrap(16, 36, done && code != 0 && !cancelled ? 0xE06040 : 0xFFFFFF, phase, 76, 10);
    int log_x = 16, log_y = 64, log_w = 76;
    if (s_qr_on && s_qr_n && !done) {
        if (s_qr_dirty) {
            if (s_qr_tex)
                SDL_DestroyTexture(s_qr_tex);
            s_qr_tex = SDL_CreateTexture(s_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, s_qr_n, s_qr_n);
            Uint32 *px = malloc((size_t)s_qr_n * (size_t)s_qr_n * 4);
            if (s_qr_tex && px) {
                for (int y = 0; y < s_qr_n; y++)
                    for (int x = 0; x < s_qr_n; x++)
                        px[y * s_qr_n + x] = s_qr[y][x] ? 0xFF000000 : 0xFFFFFFFF;
                SDL_UpdateTexture(s_qr_tex, NULL, px, s_qr_n * 4);
            }
            free(px);
            s_qr_dirty = 0;
        }
        int box = 224, module = box / s_qr_n > 0 ? box / s_qr_n : 1, size = module * s_qr_n;
        fill(16, 60, box + 8, box + 8, 0xFFFFFF); /* (el borde claro: más lugar en blanco alrededor ayuda al celular) */
        SDL_Rect dst = { 16 + 4 + (box - size) / 2, 60 + 4 + (box - size) / 2, size, size };
        if (s_qr_tex)
            SDL_RenderCopy(s_r, s_qr_tex, NULL, &dst);
        int y = text_wrap(256, 64, 0xFFE8B0,
                          TX("Abrí la app de Steam en el celular, tocá el escudo (Steam Guard) y escaneá este código. "
                             "La contraseña no se escribe acá.",
                             "Open the Steam app on your phone, tap the shield (Steam Guard) and scan this code. Your "
                             "password is not typed here."),
                          46, 11);
        log_x = 256;
        log_y = y + 10;
        log_w = 46;
    }
    for (int i = 0; i < s_nlog; i++)
        text(log_x, log_y + i * 11, 1, 0x9A8C70, s_log[i], log_w);
    if (s_progress >= 0 && !done) {
        double p = s_progress > 100 ? 100 : s_progress;
        fill(16, 296, W - 32, 14, 0x3A2C18);
        fill(18, 298, (int)((W - 36) * p / 100.0), 10, 0xC8A050);
        char pct[16];
        snprintf(pct, sizeof pct, "%.1f %%", p);
        text(W / 2 - 24, 316, 1, 0xFFFFFF, pct, 12);
    }
    text(16, H - 18, 1, 0x806040,
         done ? TX("A: salir", "A: exit") : TX("B: cancelar (la próxima vez sigue donde quedó)", "B: cancel (next time it resumes)"),
         76);
}

/* ---- El importador ---- */

static pid_t spawn(const char *importer, const char *lang, const char *pkg, int *fd)
{
    int p[2];
    if (pipe(p) != 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0); /* (su grupo: cancelar corta también a DepotDownloader) */
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        execl(importer, importer, "--steam", "--ui", "--idioma", lang, "--destino", pkg, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    *fd = p[0];
    return pid;
}

int steamui_run(const char *lang)
{
    s_es = !strncmp(lang, "es", 2);
    const char *script = getenv("D2_STEAMUI_SCRIPT"), *shot = getenv("D2_STEAMUI_SHOT");
    char dir[1024] = ".", importer[1100];
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n > 0) {
        dir[n] = '\0';
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = '\0';
    } else {
        snprintf(dir, sizeof dir, ".");
    }
    snprintf(importer, sizeof importer, "%s/d2port-importar", dir);

    if (SDL_Init(shot ? 0 : SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); /* (el QR con los bordes nítidos) */
    SDL_Window  *win = NULL;
    SDL_Surface *canvas = NULL;
    if (shot) {
        canvas = SDL_CreateRGBSurfaceWithFormat(0, W * 2, H * 2, 32, SDL_PIXELFORMAT_ARGB8888);
        s_r = canvas ? SDL_CreateSoftwareRenderer(canvas) : NULL;
    } else {
        win = SDL_CreateWindow("Diablo II - Steam", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                               SDL_WINDOW_FULLSCREEN_DESKTOP);
        s_r = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (win && !s_r)
            s_r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_r || font_make() != 0) {
        fprintf(stderr, "steamui: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(s_r, W, H);
    for (int i = 0; !shot && i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i))
            SDL_GameControllerOpen(i);
    snprintf(s_phase, sizeof s_phase, "%s", TX("Arrancando el importador...", "Starting the importer..."));

    int fd = -1, done = 0, code = 1, cancelled = 0;
    pid_t pid = -1;
    if (script) {
        fd = open(script, O_RDONLY);
    } else {
        pid = spawn(importer, lang, dir, &fd);
        if (pid < 0)
            snprintf(s_phase, sizeof s_phase, "%s: %.120s", TX("No pude correr", "Could not run"), importer);
    }
    char buf[65536], line[8192];
    size_t ln = 0;
    for (;;) {
        /* Lo que llegó del importador */
        while (fd >= 0) {
            ssize_t got = read(fd, buf, sizeof buf);
            if (got <= 0) {
                if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(fd);
                    fd = -1;
                }
                break;
            }
            for (ssize_t i = 0; i < got; i++) {
                if (buf[i] == '\n' || ln + 1 >= sizeof line) {
                    line[ln] = '\0';
                    on_line(line);
                    ln = 0;
                } else {
                    line[ln++] = buf[i];
                }
            }
        }
        if (fd < 0 && ln) {
            line[ln] = '\0';
            on_line(line);
            ln = 0;
        }
        if (!done && fd < 0) { /* terminó (o, con un guion de prueba, se acabó el archivo) */
            int status = 0;
            if (pid > 0 && waitpid(pid, &status, 0) == pid)
                code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            else if (script)
                code = 0;
            done = !shot; /* (la captura muestra cómo iba) */
            printf("steamui: el importador terminó con %d\n", code);
        }
        if (shot) {
            draw(0, code, 0);
            SDL_RenderPresent(s_r);
            SDL_SaveBMP(canvas, shot);
            break;
        }
        SDL_Event e;
        int a = 0, b = 0;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                b = 1;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN)
                a |= e.cbutton.button == SDL_CONTROLLER_BUTTON_A || e.cbutton.button == SDL_CONTROLLER_BUTTON_START,
                    b |= e.cbutton.button == SDL_CONTROLLER_BUTTON_B || e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK;
            else if (e.type == SDL_KEYDOWN)
                a |= e.key.keysym.sym == SDLK_RETURN, b |= e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_BACKSPACE;
            else if (e.type == SDL_CONTROLLERDEVICEADDED)
                SDL_GameControllerOpen(e.cdevice.which);
        }
        if (done && (a || b))
            break;
        if (!done && b && pid > 0) { /* cancelar: el importador y DepotDownloader (su grupo) */
            kill(-pid, SIGTERM);
            waitpid(pid, NULL, 0);
            if (fd >= 0)
                close(fd);
            fd = -1;
            done = cancelled = 1;
            code = 1;
            printf("steamui: cancelado\n");
        }
        draw(done, code, cancelled);
        SDL_RenderPresent(s_r);
        SDL_Delay(33);
    }
    if (s_qr_tex)
        SDL_DestroyTexture(s_qr_tex);
    SDL_DestroyTexture(s_font);
    SDL_DestroyRenderer(s_r);
    if (canvas)
        SDL_FreeSurface(canvas);
    if (win)
        SDL_DestroyWindow(win);
    SDL_Quit();
    return code;
}

#endif
