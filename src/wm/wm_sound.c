#include "wm_sound.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SFX_RATE       48000
#define SFX_CHANNELS   2
#define SFX_PERIOD     480          // 10 ms a 48 kHz (coincide con dmix de /etc/asound.conf)
#define SFX_MAX_VOICES 8
#define SFX_IDLE_MS    2500         // cierra ALSA tras 2,5 s en silencio para no ocupar recursos

static const char *const SFX_NAMES[WM_SFX_COUNT] = {
    "tick", "edge", "select", "back", "home", "page", "close",
};

/* ------------------------------------------------ síntesis estilo Switch */

static inline int16_t clamp_s16(double v) {
    if (v > 1.0) v = 1.0;
    if (v < -1.0) v = -1.0;
    return (int16_t)(v * 32767.0);
}

static double env_exp(double t, double attack, double decay) {
    if (t < 0.0) return 0.0;
    double a = (attack > 0.0 && t < attack) ? (t / attack) : 1.0;
    return a * exp(-t / decay);
}

static double render_sample(enum wm_sfx sfx, double t) {
    switch (sfx) {
        case WM_SFX_TICK: {
            // Click táctil corto de madera/cristal (~24 ms)
            double f = 820.0 + 540.0 * exp(-t / 0.0045);
            double body = sin(2.0 * M_PI * f * t) * env_exp(t, 0.0006, 0.0075);
            double click = sin(2.0 * M_PI * 2650.0 * t) * env_exp(t, 0.0003, 0.0028);
            return 0.34 * (0.72 * body + 0.38 * click);
        }
        case WM_SFX_EDGE: {
            // Tope grave amortiguado (~36 ms)
            double f = 145.0 + 115.0 * exp(-t / 0.008);
            double body = sin(2.0 * M_PI * f * t) * env_exp(t, 0.001, 0.012);
            double sub = sin(2.0 * M_PI * 95.0 * t) * env_exp(t, 0.001, 0.015);
            return 0.32 * (0.75 * body + 0.35 * sub);
        }
        case WM_SFX_SELECT: {
            // Click brillante + doble nota ascendente E5 -> B5 (~120 ms)
            double click = sin(2.0 * M_PI * 2400.0 * t) * env_exp(t, 0.0004, 0.003);
            double n1 = (sin(2.0 * M_PI * 659.25 * t) + 0.25 * sin(2.0 * M_PI * 1318.5 * t))
                        * env_exp(t, 0.0015, 0.028);
            double t2 = t - 0.028;
            double n2 = (t2 >= 0.0)
                ? (sin(2.0 * M_PI * 987.77 * t2) + 0.22 * sin(2.0 * M_PI * 1975.53 * t2))
                  * env_exp(t2, 0.0015, 0.036)
                : 0.0;
            return 0.30 * (0.30 * click + 0.55 * n1 + 0.72 * n2);
        }
        case WM_SFX_BACK: {
            // Descenso limpio G5 -> D5 (~85 ms)
            double n1 = sin(2.0 * M_PI * 783.99 * t) * env_exp(t, 0.0012, 0.020);
            double t2 = t - 0.022;
            double n2 = (t2 >= 0.0)
                ? (sin(2.0 * M_PI * 587.33 * t2) + 0.15 * sin(2.0 * M_PI * 1174.66 * t2))
                  * env_exp(t2, 0.0012, 0.026)
                : 0.0;
            return 0.28 * (0.60 * n1 + 0.65 * n2);
        }
        case WM_SFX_HOME: {
            // Acorde suave tipo campanilla C5 + G5 + C6 (~135 ms)
            double c5 = sin(2.0 * M_PI * 523.25 * t) * env_exp(t, 0.002, 0.048);
            double t2 = t - 0.014;
            double g5 = (t2 >= 0.0) ? sin(2.0 * M_PI * 783.99 * t2) * env_exp(t2, 0.002, 0.045) : 0.0;
            double t3 = t - 0.026;
            double c6 = (t3 >= 0.0) ? sin(2.0 * M_PI * 1046.50 * t3) * env_exp(t3, 0.002, 0.038) : 0.0;
            return 0.28 * (0.50 * c5 + 0.50 * g5 + 0.28 * c6);
        }
        case WM_SFX_PAGE: {
            // Cambio de página / abrir carpeta: F5 -> A5 (~72 ms)
            double n1 = sin(2.0 * M_PI * 698.46 * t) * env_exp(t, 0.001, 0.018);
            double t2 = t - 0.020;
            double n2 = (t2 >= 0.0) ? sin(2.0 * M_PI * 880.00 * t2) * env_exp(t2, 0.001, 0.024) : 0.0;
            return 0.28 * (0.55 * n1 + 0.65 * n2);
        }
        case WM_SFX_CLOSE: {
            // Cerrar app: E5 -> A4 suave (~90 ms)
            double n1 = sin(2.0 * M_PI * 659.25 * t) * env_exp(t, 0.0012, 0.022);
            double t2 = t - 0.024;
            double n2 = (t2 >= 0.0) ? sin(2.0 * M_PI * 440.00 * t2) * env_exp(t2, 0.0015, 0.030) : 0.0;
            return 0.28 * (0.55 * n1 + 0.68 * n2);
        }
        default:
            return 0.0;
    }
}

static double sfx_duration_s(enum wm_sfx sfx) {
    switch (sfx) {
        case WM_SFX_TICK:   return 0.026;
        case WM_SFX_EDGE:   return 0.038;
        case WM_SFX_SELECT: return 0.125;
        case WM_SFX_BACK:   return 0.088;
        case WM_SFX_HOME:   return 0.140;
        case WM_SFX_PAGE:   return 0.075;
        case WM_SFX_CLOSE:  return 0.095;
        default:            return 0.0;
    }
}

size_t wm_sound_synth(enum wm_sfx sfx, int16_t **out_stereo_48k) {
    if (!out_stereo_48k || sfx < 0 || sfx >= WM_SFX_COUNT) return 0;
    size_t frames = (size_t)(sfx_duration_s(sfx) * SFX_RATE);
    if (frames == 0) return 0;
    int16_t *buf = malloc(frames * SFX_CHANNELS * sizeof(int16_t));
    if (!buf) return 0;
    size_t fade = frames > 96 ? 96 : frames;
    for (size_t i = 0; i < frames; i++) {
        double t = (double)i / (double)SFX_RATE;
        double s = render_sample(sfx, t);
        if (i >= frames - fade) {
            s *= (double)(frames - i) / (double)fade;
        }
        int16_t q = clamp_s16(s);
        buf[i * 2] = q;
        buf[i * 2 + 1] = q;
    }
    *out_stereo_48k = buf;
    return frames;
}

/* ------------------------------------------- cargador WAV PCM 16-bit */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t wm_sound_load_wav(const char *path, int16_t **out_stereo_48k) {
    if (!path || !out_stereo_48k) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 44 || sz > 4 * 1024 * 1024) { fclose(f); return 0; }
    rewind(f);
    uint8_t *raw = malloc((size_t)sz);
    if (!raw) { fclose(f); return 0; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return 0; }
    fclose(f);

    if (memcmp(raw, "RIFF", 4) != 0 || memcmp(raw + 8, "WAVE", 4) != 0) { free(raw); return 0; }

    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    size_t pos = 12;
    while (pos + 8 <= (size_t)sz) {
        uint32_t clen = rd_u32(raw + pos + 4);
        if (pos + 8 + clen > (size_t)sz) break;
        if (memcmp(raw + pos, "fmt ", 4) == 0 && clen >= 16) {
            fmt = rd_u16(raw + pos + 8);
            ch = rd_u16(raw + pos + 10);
            rate = rd_u32(raw + pos + 12);
            bits = rd_u16(raw + pos + 22);
        } else if (memcmp(raw + pos, "data", 4) == 0) {
            data = raw + pos + 8;
            data_len = clen;
        }
        pos += 8 + clen + (clen & 1u);
    }

    if (fmt != 1 || bits != 16 || (ch != 1 && ch != 2) || rate < 8000 || rate > 192000 || !data || data_len < 4) {
        free(raw);
        return 0;
    }

    size_t in_frames = data_len / (size_t)(ch * 2);
    size_t out_frames = (size_t)(((double)in_frames * SFX_RATE) / (double)rate);
    if (out_frames == 0 || out_frames > SFX_RATE * 5) { free(raw); return 0; }

    int16_t *out = malloc(out_frames * SFX_CHANNELS * sizeof(int16_t));
    if (!out) { free(raw); return 0; }
    const int16_t *in = (const int16_t *)data;
    double step = (double)rate / (double)SFX_RATE;
    for (size_t i = 0; i < out_frames; i++) {
        double src = i * step;
        size_t i0 = (size_t)src;
        if (i0 >= in_frames) i0 = in_frames - 1;
        size_t i1 = (i0 + 1 < in_frames) ? i0 + 1 : i0;
        double frac = src - (double)i0;
        if (ch == 1) {
            double v = in[i0] * (1.0 - frac) + in[i1] * frac;
            int16_t q = (int16_t)v;
            out[i * 2] = q;
            out[i * 2 + 1] = q;
        } else {
            double l = in[i0 * 2] * (1.0 - frac) + in[i1 * 2] * frac;
            double r = in[i0 * 2 + 1] * (1.0 - frac) + in[i1 * 2 + 1] * frac;
            out[i * 2] = (int16_t)l;
            out[i * 2 + 1] = (int16_t)r;
        }
    }
    free(raw);
    *out_stereo_48k = out;
    return out_frames;
}

/* ------------------------------------------- salida ALSA por dlopen */

typedef struct _snd_pcm snd_pcm_t;
typedef int (*fn_pcm_open)(snd_pcm_t **, const char *, int, int);
typedef int (*fn_pcm_close)(snd_pcm_t *);
typedef int (*fn_pcm_set_params)(snd_pcm_t *, int, int, unsigned int, unsigned int, int, unsigned int);
typedef long (*fn_pcm_writei)(snd_pcm_t *, const void *, unsigned long);
typedef int (*fn_pcm_recover)(snd_pcm_t *, int, int);
typedef int (*fn_pcm_prepare)(snd_pcm_t *);

#define ALSA_STREAM_PLAYBACK 0
#define ALSA_FORMAT_S16_LE   2
#define ALSA_ACCESS_RW_INT   3

struct sfx_clip {
    int16_t *pcm;
    size_t frames;
};

struct sfx_voice {
    enum wm_sfx sfx;
    size_t pos;
    bool active;
};

static struct {
    bool inited;
    bool running;
    bool silent;
    void *dl;
    fn_pcm_open pcm_open;
    fn_pcm_close pcm_close;
    fn_pcm_set_params pcm_set_params;
    fn_pcm_writei pcm_writei;
    fn_pcm_recover pcm_recover;
    fn_pcm_prepare pcm_prepare;

    struct sfx_clip clips[WM_SFX_COUNT];
    struct sfx_voice voices[SFX_MAX_VOICES];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool thread_started;
} g_snd;

static bool load_alsa(void) {
    g_snd.dl = dlopen("libasound.so.2", RTLD_LAZY);
    if (!g_snd.dl) return false;
    g_snd.pcm_open = (fn_pcm_open)dlsym(g_snd.dl, "snd_pcm_open");
    g_snd.pcm_close = (fn_pcm_close)dlsym(g_snd.dl, "snd_pcm_close");
    g_snd.pcm_set_params = (fn_pcm_set_params)dlsym(g_snd.dl, "snd_pcm_set_params");
    g_snd.pcm_writei = (fn_pcm_writei)dlsym(g_snd.dl, "snd_pcm_writei");
    g_snd.pcm_recover = (fn_pcm_recover)dlsym(g_snd.dl, "snd_pcm_recover");
    g_snd.pcm_prepare = (fn_pcm_prepare)dlsym(g_snd.dl, "snd_pcm_prepare");
    if (!g_snd.pcm_open || !g_snd.pcm_close || !g_snd.pcm_set_params || !g_snd.pcm_writei) {
        dlclose(g_snd.dl);
        g_snd.dl = NULL;
        return false;
    }
    return true;
}

static snd_pcm_t *open_pcm(void) {
    snd_pcm_t *pcm = NULL;
    if (g_snd.pcm_open(&pcm, "default", ALSA_STREAM_PLAYBACK, 0) < 0 || !pcm) return NULL;
    // 40 ms de latencia total sobre dmix: respuesta rápida sin xruns
    if (g_snd.pcm_set_params(pcm, ALSA_FORMAT_S16_LE, ALSA_ACCESS_RW_INT,
                             SFX_CHANNELS, SFX_RATE, 1, 40000) < 0) {
        g_snd.pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

static bool has_active_voice_locked(void) {
    for (int i = 0; i < SFX_MAX_VOICES; i++)
        if (g_snd.voices[i].active) return true;
    return false;
}

static void *mixer_loop(void *arg) {
    (void)arg;
    snd_pcm_t *pcm = NULL;
    int16_t mix[SFX_PERIOD * SFX_CHANNELS];
    int idle_periods = 0;
    const int max_idle_periods = (SFX_IDLE_MS * (SFX_RATE / 1000)) / SFX_PERIOD;

    for (;;) {
        pthread_mutex_lock(&g_snd.mu);
        while (g_snd.running && !has_active_voice_locked() && !pcm) {
            pthread_cond_wait(&g_snd.cv, &g_snd.mu);
        }
        if (!g_snd.running) {
            pthread_mutex_unlock(&g_snd.mu);
            break;
        }
        bool any = false;
        memset(mix, 0, sizeof(mix));
        for (int v = 0; v < SFX_MAX_VOICES; v++) {
            if (!g_snd.voices[v].active) continue;
            struct sfx_clip *c = &g_snd.clips[g_snd.voices[v].sfx];
            size_t rem = c->frames - g_snd.voices[v].pos;
            size_t n = rem < SFX_PERIOD ? rem : SFX_PERIOD;
            const int16_t *src = c->pcm + g_snd.voices[v].pos * SFX_CHANNELS;
            for (size_t i = 0; i < n * SFX_CHANNELS; i++) {
                int32_t acc = (int32_t)mix[i] + (int32_t)src[i];
                if (acc > 32767) acc = 32767;
                else if (acc < -32768) acc = -32768;
                mix[i] = (int16_t)acc;
            }
            g_snd.voices[v].pos += n;
            if (g_snd.voices[v].pos >= c->frames) g_snd.voices[v].active = false;
            any = true;
        }
        pthread_mutex_unlock(&g_snd.mu);

        if (any) {
            idle_periods = 0;
            if (!pcm) pcm = open_pcm();
        } else {
            idle_periods++;
            if (pcm && idle_periods >= max_idle_periods) {
                g_snd.pcm_close(pcm);
                pcm = NULL;
                idle_periods = 0;
                continue;
            }
        }

        if (!pcm) {
            // Si ALSA falló al abrir, vaciar voces para no girar en bucle
            pthread_mutex_lock(&g_snd.mu);
            for (int v = 0; v < SFX_MAX_VOICES; v++) g_snd.voices[v].active = false;
            pthread_mutex_unlock(&g_snd.mu);
            continue;
        }

        long wr = g_snd.pcm_writei(pcm, mix, SFX_PERIOD);
        if (wr < 0) {
            int rec = g_snd.pcm_recover ? g_snd.pcm_recover(pcm, (int)wr, 1) : -1;
            if (rec < 0) {
                g_snd.pcm_close(pcm);
                pcm = NULL;
            }
        }
    }

    if (pcm) g_snd.pcm_close(pcm);
    return NULL;
}

static void load_clips(void) {
    const char *home = getenv("HOME");
    for (int i = 0; i < WM_SFX_COUNT; i++) {
        char path[512];
        size_t n = 0;
        if (home && home[0]) {
            snprintf(path, sizeof(path), "%s/.config/wiiu-shell/sounds/%s.wav", home, SFX_NAMES[i]);
            n = wm_sound_load_wav(path, &g_snd.clips[i].pcm);
        }
        if (n == 0) {
            snprintf(path, sizeof(path), "/etc/wiiu-shell/sounds/%s.wav", SFX_NAMES[i]);
            n = wm_sound_load_wav(path, &g_snd.clips[i].pcm);
        }
        if (n == 0) {
            n = wm_sound_synth((enum wm_sfx)i, &g_snd.clips[i].pcm);
        }
        g_snd.clips[i].frames = n;
    }
}

void wm_sound_init(void) {
    if (g_snd.inited) return;
    g_snd.inited = true;
    const char *sil = getenv("WIIU_WM_SILENT");
    if (sil && strcmp(sil, "1") == 0) {
        g_snd.silent = true;
        return;
    }
    if (!load_alsa()) {
        g_snd.silent = true;
        return;
    }
    load_clips();
    pthread_mutex_init(&g_snd.mu, NULL);
    pthread_cond_init(&g_snd.cv, NULL);
    g_snd.running = true;
    if (pthread_create(&g_snd.thread, NULL, mixer_loop, NULL) == 0) {
        g_snd.thread_started = true;
    } else {
        g_snd.running = false;
        g_snd.silent = true;
    }
}

void wm_sound_play(enum wm_sfx sfx) {
    if (!g_snd.inited || g_snd.silent || sfx < 0 || sfx >= WM_SFX_COUNT) return;
    if (!g_snd.clips[sfx].pcm || g_snd.clips[sfx].frames == 0) return;
    pthread_mutex_lock(&g_snd.mu);
    int slot = -1;
    for (int i = 0; i < SFX_MAX_VOICES; i++) {
        if (!g_snd.voices[i].active) { slot = i; break; }
    }
    if (slot < 0) slot = 0; // si se saturan las 8 voces, reutiliza la más antigua
    g_snd.voices[slot].sfx = sfx;
    g_snd.voices[slot].pos = 0;
    g_snd.voices[slot].active = true;
    pthread_cond_signal(&g_snd.cv);
    pthread_mutex_unlock(&g_snd.mu);
}

void wm_sound_finish(void) {
    if (!g_snd.inited) return;
    if (g_snd.thread_started) {
        pthread_mutex_lock(&g_snd.mu);
        g_snd.running = false;
        pthread_cond_signal(&g_snd.cv);
        pthread_mutex_unlock(&g_snd.mu);
        pthread_join(g_snd.thread, NULL);
        g_snd.thread_started = false;
        pthread_mutex_destroy(&g_snd.mu);
        pthread_cond_destroy(&g_snd.cv);
    }
    for (int i = 0; i < WM_SFX_COUNT; i++) {
        free(g_snd.clips[i].pcm);
        g_snd.clips[i].pcm = NULL;
        g_snd.clips[i].frames = 0;
    }
    if (g_snd.dl) {
        dlclose(g_snd.dl);
        g_snd.dl = NULL;
    }
    g_snd.inited = false;
}
