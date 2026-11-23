// Reloj de fotogramas propio del WM (B19).
//
// En la salida headless de wlroots el siguiente evento «frame» se programa 16 ms después de cada
// commit, y el commit sale tras dibujar la escena y capturarla para drc_stream (~8 ms): el periodo
// real era ~24 ms (41 Hz). RetroArch con vsync espera ese evento, así que corría a 41 fps y su
// audio no llegaba a tiempo (cortes). Con el reloj activo, un timerfd periódico (rejilla fija del
// núcleo, sin deriva) pide cada fotograma y se ignoran los eventos de la salida headless.
#pragma once

#include <stdbool.h>

#define WM_FRAMECLOCK_PERIOD_NS 16683333L   // 59,94 Hz (igual que el GamePad y libdrc)

typedef struct {
    bool active;     // hay ventanas: el ritmo lo marca el reloj
    bool due;        // un tick pidió fotograma y aún no se atendió
    long ticks, accepted, ignored;
} wm_frameclock;

void wm_frameclock_init(wm_frameclock *fc);
// Devuelve true al pasar de activo a inactivo: el llamador debe relanzar la salida (el último evento «frame»
// de la salida headless lo descartó el reloj y nadie pediría otro: imagen congelada).
bool wm_frameclock_set_active(wm_frameclock *fc, bool active);
// Un vencimiento del timerfd: pide un fotograma (los que no se atienden no se acumulan).
void wm_frameclock_tick(wm_frameclock *fc);
// Llega un evento «frame» de la salida: ¿se dibuja? Inactivo, siempre; activo, solo si lo pidió un tick.
bool wm_frameclock_accept(wm_frameclock *fc);
