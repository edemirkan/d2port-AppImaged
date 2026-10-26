#ifndef STEAMUI_H
#define STEAMUI_H

/* La pantalla de "Importar desde Steam" (d2port --importar-steam <idioma>, docs/steam.md): corre el importador con
   --steam --ui y muestra en la tele el código QR para iniciar sesión, el avance y los mensajes. Para la Pi (en la PC
   el importador corre en una consola). Devuelve el código de salida del importador. */
int steamui_run(const char *lang);

#endif
