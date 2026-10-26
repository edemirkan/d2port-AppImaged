cmake_minimum_required(VERSION 3.16)
project(d2port C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# La red local entre la Pi, Linux y Windows: todas tienen que calcular exactamente lo mismo, y el ARM
# junta multiplicación y suma en una sola operación (FMA) que redondea distinto: sin eso, los mismos
# números en las tres. Y char con signo, como en x86 (en ARM es sin signo). Las funciones de math.h
# que usa la simulación son las de src/dmath.h (las del sistema difieren en el último bit).
add_compile_options(-ffp-contract=off -fsigned-char)

# La versión del código (la red solo junta máquinas con la misma): el commit de git, o "dev"
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                OUTPUT_VARIABLE D2_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT D2_GIT_VERSION)
  set(D2_GIT_VERSION dev)
endif()

if(NOT EMSCRIPTEN)
  find_package(SDL2 REQUIRED)
endif()

# En la Pi (Cortex-A53, que ejecuta en orden) la vectorización SLP de GCC
# arma los colores de la conversión del cuadro con NEON pasando por la pila:
# sin ella la conversión tarda 2,6 ms en vez de 3,6 y el resto no cambia
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm")
  add_compile_options(-fno-tree-slp-vectorize)
endif()

# Lectores de formatos de Diablo II, compartidos por el motor y las herramientas
add_library(d2fmtlib STATIC
  src/fmt/fs.c
  src/fmt/sprite.c
  src/fmt/animdata.c
  src/fmt/txt.c
  src/fmt/dcc.c
  src/fmt/cof.c
  src/fmt/dt1.c
  src/fmt/ds1.c
  src/fmt/flac.c
  src/fmt/ttf.c
)
target_compile_options(d2fmtlib PRIVATE -Wall -Wextra)

add_executable(d2port
  src/main.c
  src/video.c
  src/input.c
  src/font.c
  src/ui.c
  src/charanim.c
  src/map.c
  src/world.c
  src/jobs.c
  src/bot.c
  src/strings.c
  src/lang.c
  src/items.c
  src/itemstats.c
  src/cube.c
  src/skills.c
  src/sequences.c
  src/missiles.c
  src/overlays.c
  src/audio.c
  src/cinema.c
  src/units.c
  src/menu.c
  src/controls.c
  src/game.c
  src/net.c
  src/compat.c
  src/dmath.c
  src/steamui.c
)
target_compile_options(d2port PRIVATE -Wall -Wextra)
target_compile_definitions(d2port PRIVATE D2_VERSION="${D2_GIT_VERSION}")
if(EMSCRIPTEN)
  # El navegador (emcmake cmake -S . -B build-web; docs/web.md): el SDL2 de Emscripten, ASYNCIFY para que
  # los bucles del juego y la lectura de los archivos (que se piden al servidor) puedan esperar sin trabar la
  # página, las partidas en el almacenamiento del navegador (IDBFS) si no está d2port-servidor, y la red local por
  # d2port-servidor (web/red.js). Sale d2port.js + d2port.wasm, que carga web/index.html.
  target_compile_options(d2port PRIVATE "SHELL:-s USE_SDL=2")
  target_compile_options(d2fmtlib PRIVATE "SHELL:-s USE_SDL=2")
  target_link_libraries(d2port PRIVATE d2fmtlib m idbfs.js)
  target_link_options(d2port PRIVATE "SHELL:-s USE_SDL=2" "SHELL:-s ASYNCIFY=1" "SHELL:-s ASYNCIFY_STACK_SIZE=1048576"
                      "SHELL:-s ALLOW_MEMORY_GROWTH=1" "SHELL:-s INITIAL_MEMORY=268435456" "SHELL:-s STACK_SIZE=2097152"
                      "SHELL:-s FORCE_FILESYSTEM=1" "SHELL:-s ENVIRONMENT=web" "SHELL:-s EXIT_RUNTIME=0"
                      "SHELL:-s EXPORTED_RUNTIME_METHODS=['FS','IDBFS','ENV','callMain']" "SHELL:-s INVOKE_RUN=0"
                      "SHELL:--pre-js \"${CMAKE_SOURCE_DIR}/web/cache.js\"" "SHELL:--pre-js \"${CMAKE_SOURCE_DIR}/web/red.js\"")
  set_property(TARGET d2port APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/cache.js;${CMAKE_SOURCE_DIR}/web/red.js")
  set_target_properties(d2port PROPERTIES SUFFIX ".js")
elseif(WIN32)
  # Windows (cmake/mingw-w64.cmake): el main de SDL (antes que SDL2), clock_gettime de winpthread, sin
  # ventana de consola y sin las DLL de MinGW (al lado del .exe solo va SDL2.dll)
  target_link_libraries(d2port PRIVATE d2fmtlib SDL2::SDL2main SDL2::SDL2 m pthread ws2_32 iphlpapi)
  target_link_options(d2port PRIVATE -mwindows -static-libgcc -static)
else()
  target_link_libraries(d2port PRIVATE d2fmtlib SDL2::SDL2 m)
endif()

add_executable(d2fmt tools/d2fmt.c)
target_link_libraries(d2fmt PRIVATE d2fmtlib)
target_compile_options(d2fmt PRIVATE -Wall -Wextra)

# Extractor del CASC del Resurrected (solo en la PC, necesita CascLib compilada)
set(CASCLIB_DIR "$ENV{HOME}/src/CascLib" CACHE PATH "Carpeta de CascLib compilada")
if(EXISTS "${CASCLIB_DIR}/build/libcasc.a" AND NOT CMAKE_CROSSCOMPILING)
  find_package(ZLIB REQUIRED)
  add_executable(d2rextract tools/d2rextract.c)
  target_include_directories(d2rextract PRIVATE "${CASCLIB_DIR}/src")
  target_link_libraries(d2rextract PRIVATE "${CASCLIB_DIR}/build/libcasc.a" ZLIB::ZLIB stdc++ pthread)
  # El importador: copia de tu Diablo II: Resurrected los archivos que usa el
  # port (tools/archivos-del-juego.txt) a data/ (./build/d2port-importar desde acá)
  add_executable(d2port-importar tools/d2port-importar.c)
  target_include_directories(d2port-importar PRIVATE "${CASCLIB_DIR}/src")
  target_compile_definitions(d2port-importar PRIVATE CASCLIB_NO_AUTO_LINK_LIBRARY CASCLIB_NODEBUG)
  target_compile_options(d2port-importar PRIVATE -Wall -Wextra)
  target_link_libraries(d2port-importar PRIVATE "${CASCLIB_DIR}/build/libcasc.a" ZLIB::ZLIB stdc++ pthread)
endif()
