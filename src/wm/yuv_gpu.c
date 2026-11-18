#include "yuv_gpu.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 854
#define SH 480
#define TEXW 214              /* 856 bytes por fila (4 por texel) */
#define TEXH (SH + SH / 2)    /* 480 filas de luma + 240 de croma */
#define ROWB (TEXW * 4)

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001
#endif
typedef void *(*map_range_fn)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLboolean (*unmap_fn)(GLenum);

struct yuv_gpu {
    GLuint pbo;                       /* lectura asíncrona; 0 = glReadPixels directo */
    map_range_fn map_range;
    unmap_fn unmap;
    GLuint fbo, target, vbo;
    GLuint prog2d, progext;           /* uno por tipo de textura de origen */
    GLint loc2d, locext;
    uint8_t *rb;                      /* lectura: TEXW*4 * TEXH */
};

static const char *VS =
    "attribute vec2 p;\n"
    "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";

/* %s: directiva de extensión (vacía o EGL_image_external) y tipo de sampler */
static const char *FS_FMT =
    "%s"
    "precision highp float;\n"
    "uniform %s src;\n"
    "vec3 px(float x, float y) {\n"
    "  return texture2D(src, vec2((min(x, 853.0) + 0.5) / 854.0, (min(y, 479.0) + 0.5) / 480.0)).rgb * 255.0;\n"
    "}\n"
    "float lum(vec3 c) { return floor((66.0 * c.r + 129.0 * c.g + 25.0 * c.b + 128.0) / 256.0) + 16.0; }\n"
    "void main() {\n"
    "  float tx = floor(gl_FragCoord.x);\n"
    "  float ty = floor(gl_FragCoord.y);\n"
    "  vec4 o = vec4(0.0);\n"
    "  if (ty < 480.0) {\n"
    "    for (int k = 0; k < 4; k++) o[k] = lum(px(tx * 4.0 + float(k), ty));\n"
    "  } else {\n"
    "    float cy = ty - 480.0;\n"
    "    float isv = tx >= 107.0 ? 1.0 : 0.0;\n"
    "    float cx0 = (tx - isv * 107.0) * 4.0;\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "      float cx = min(cx0 + float(k), 426.0);\n"
    "      vec3 a = px(2.0 * cx, 2.0 * cy) + px(2.0 * cx + 1.0, 2.0 * cy) +\n"
    "               px(2.0 * cx, 2.0 * cy + 1.0) + px(2.0 * cx + 1.0, 2.0 * cy + 1.0);\n"
    "      a = floor((a + 2.0) / 4.0);\n"
    "      o[k] = isv > 0.5 ? floor((112.0 * a.r - 94.0 * a.g - 18.0 * a.b + 128.0) / 256.0) + 128.0\n"
    "                       : floor((-38.0 * a.r - 74.0 * a.g + 112.0 * a.b + 128.0) / 256.0) + 128.0;\n"
    "    }\n"
    "  }\n"
    "  gl_FragColor = o / 255.0;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[yuv_gpu] error de shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint program(const char *ext, const char *sampler, GLint *loc) {
    char fs[4096];
    snprintf(fs, sizeof(fs), FS_FMT, ext, sampler);
    GLuint v = compile(GL_VERTEX_SHADER, VS), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "p");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    *loc = glGetUniformLocation(p, "src");
    return p;
}

yuv_gpu *yuv_gpu_create(void) {
    yuv_gpu *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->prog2d = program("", "sampler2D", &g->loc2d);
    // El de textura externa solo se usa si wlroots importa el búfer como GL_TEXTURE_EXTERNAL_OES
    g->progext = program("#extension GL_OES_EGL_image_external : require\n", "samplerExternalOES", &g->locext);
    if (!g->prog2d) { free(g); return NULL; }
    glGenTextures(1, &g->target);
    glBindTexture(GL_TEXTURE_2D, g->target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEXW, TEXH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &g->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->target, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glGenBuffers(1, &g->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    g->rb = malloc((size_t)ROWB * TEXH);
    g->map_range = (map_range_fn)eglGetProcAddress("glMapBufferRange");
    g->unmap = (unmap_fn)eglGetProcAddress("glUnmapBuffer");
    if (g->map_range && g->unmap && !getenv("WIIU_CAPTURE_NOPBO")) {
        glGenBuffers(1, &g->pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g->pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)ROWB * TEXH, NULL, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (glGetError() != GL_NO_ERROR) { glDeleteBuffers(1, &g->pbo); g->pbo = 0; }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (!ok || !g->rb) { yuv_gpu_destroy(g); return NULL; }
    return g;
}

void yuv_gpu_destroy(yuv_gpu *g) {
    if (!g) return;
    if (g->fbo) glDeleteFramebuffers(1, &g->fbo);
    if (g->target) glDeleteTextures(1, &g->target);
    if (g->vbo) glDeleteBuffers(1, &g->vbo);
    if (g->pbo) glDeleteBuffers(1, &g->pbo);
    if (g->prog2d) glDeleteProgram(g->prog2d);
    if (g->progext) glDeleteProgram(g->progext);
    free(g->rb);
    free(g);
}

bool yuv_gpu_convert(yuv_gpu *g, unsigned tex, unsigned target, int y0, int y1,
                     uint8_t *y, uint8_t *u, uint8_t *v) {
    if (!g || y0 < 0 || y1 > SH || y1 <= y0 || (y0 & 1) || (y1 & 1)) return false;
    GLuint prog = target == GL_TEXTURE_EXTERNAL_OES ? g->progext : g->prog2d;
    GLint loc = target == GL_TEXTURE_EXTERNAL_OES ? g->locext : g->loc2d;
    if (!prog) return false;

    // Estado de GL que wlroots espera encontrar tal cual: se guarda y se restaura
    GLint s_prog, s_fbo, s_vp[4], s_abuf, s_atex, s_tex2d, s_texext = 0;
    GLboolean s_blend = glIsEnabled(GL_BLEND), s_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s_prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_fbo);
    glGetIntegerv(GL_VIEWPORT, s_vp);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s_abuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s_atex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s_tex2d);
    if (target == GL_TEXTURE_EXTERNAL_OES) glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &s_texext);

    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, TEXW, TEXH);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(prog);
    glBindTexture(target, tex);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(loc, 0);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);

    // Lectura solo de las bandas pedidas: luma [y0, y1) y croma [y0/2, y1/2) (a partir de la fila 480)
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    GLint s_pbo = 0;
    if (g->pbo) {
        // Con un PBO la copia (y el reordenado de la memoria de la GPU) la hace la GPU y no la CPU del controlador
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s_pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g->pbo);
        glReadPixels(0, y0, TEXW, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, (void *)((size_t)y0 * ROWB));
        glReadPixels(0, SH + y0 / 2, TEXW, (y1 - y0) / 2, GL_RGBA, GL_UNSIGNED_BYTE, (void *)((size_t)(SH + y0 / 2) * ROWB));
        const uint8_t *m = g->map_range(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)ROWB * TEXH, GL_MAP_READ_BIT);
        if (m) {
            memcpy(g->rb + (size_t)y0 * ROWB, m + (size_t)y0 * ROWB, (size_t)(y1 - y0) * ROWB);
            memcpy(g->rb + (size_t)(SH + y0 / 2) * ROWB, m + (size_t)(SH + y0 / 2) * ROWB, (size_t)((y1 - y0) / 2) * ROWB);
            g->unmap(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)s_pbo);
    } else {
        glReadPixels(0, y0, TEXW, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, g->rb + (size_t)y0 * ROWB);
        glReadPixels(0, SH + y0 / 2, TEXW, (y1 - y0) / 2, GL_RGBA, GL_UNSIGNED_BYTE, g->rb + (size_t)(SH + y0 / 2) * ROWB);
    }

    // Restaurar
    glBindBuffer(GL_ARRAY_BUFFER, s_abuf);
    glBindTexture(target, target == GL_TEXTURE_EXTERNAL_OES ? (GLuint)s_texext : (GLuint)s_tex2d);
    if (target == GL_TEXTURE_EXTERNAL_OES) { glBindTexture(GL_TEXTURE_2D, s_tex2d); }
    glActiveTexture(s_atex);
    glUseProgram(s_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(s_vp[0], s_vp[1], s_vp[2], s_vp[3]);
    if (s_blend) glEnable(GL_BLEND);
    if (s_scissor) glEnable(GL_SCISSOR_TEST);

    for (int r = y0; r < y1; r++) memcpy(y + (size_t)r * SW, g->rb + (size_t)r * ROWB, SW);
    for (int r = y0 / 2; r < y1 / 2; r++) {
        const uint8_t *row = g->rb + (size_t)(SH + r) * ROWB;
        memcpy(u + (size_t)r * (SW / 2), row, SW / 2);
        memcpy(v + (size_t)r * (SW / 2), row + 428, SW / 2);
    }
    return true;
}
