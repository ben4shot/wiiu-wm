#ifndef WM_SOUND_H
#define WM_SOUND_H

#include <stddef.h>
#include <stdint.h>

// Efectos de sonido de la interfaz de wiiu-wm (estilo táctil tipo Switch).
// Por defecto se sintetizan en memoria a 48 kHz estéreo; si existe un WAV PCM
// de 16 bits en ~/.config/wiiu-shell/sounds/<nombre>.wav o /etc/wiiu-shell/sounds/<nombre>.wav,
// sustituye automáticamente al sintetizado.
enum wm_sfx {
    WM_SFX_TICK = 0,    // tick.wav   - mover cursor (grid, cajón, recientes)
    WM_SFX_EDGE,        // edge.wav   - tope de navegación
    WM_SFX_SELECT,      // select.wav - abrir app / confirmar
    WM_SFX_BACK,        // back.wav   - volver / cerrar menú
    WM_SFX_HOME,        // home.wav   - botón HOME (Recientes / Inicio)
    WM_SFX_PAGE,        // page.wav   - cambiar página / abrir carpeta o cajón
    WM_SFX_CLOSE,       // close.wav  - cerrar ventana
    WM_SFX_COUNT,
};

void wm_sound_init(void);
void wm_sound_play(enum wm_sfx sfx);
void wm_sound_finish(void);

// Helpers expuestos para pruebas unitarias (sin abrir ALSA)
size_t wm_sound_synth(enum wm_sfx sfx, int16_t **out_stereo_48k);
size_t wm_sound_load_wav(const char *path, int16_t **out_stereo_48k);

#endif
