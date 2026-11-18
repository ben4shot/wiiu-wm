#include "yuv_convert.h"

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

__attribute__((optimize("O3")))
void xrgb_to_i420_rect(const uint8_t *src, int stride, int w, int h, int x0, int y0, int x1, int y1,
                       uint8_t *dy, uint8_t *du, uint8_t *dv) {
    (void)h;
    for (int y = y0; y < y1; y += 2) {
        const uint8_t *r0 = src + (long)y * stride, *r1 = r0 + stride;
        uint8_t *py0 = dy + (long)y * w, *py1 = py0 + w;
        uint8_t *pu = du + (long)(y / 2) * (w / 2), *pv = dv + (long)(y / 2) * (w / 2);
        for (int x = x0; x < x1; x += 2) {
            const uint8_t *a = r0 + x * 4, *b = r1 + x * 4;
            int b00 = a[0], g00 = a[1], r00 = a[2], b01 = a[4], g01 = a[5], r01 = a[6];
            int b10 = b[0], g10 = b[1], r10 = b[2], b11 = b[4], g11 = b[5], r11 = b[6];
            py0[x]    = (uint8_t)(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16);
            py0[x + 1] = (uint8_t)(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16);
            py1[x]    = (uint8_t)(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16);
            py1[x + 1] = (uint8_t)(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16);
            int rr = (r00 + r01 + r10 + r11 + 2) >> 2;
            int gg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int bb = (b00 + b01 + b10 + b11 + 2) >> 2;
            pu[x / 2] = clamp8(((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128);
            pv[x / 2] = clamp8(((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128);
        }
    }
}

void xrgb_to_i420(const uint8_t *src, int stride, int w, int h, uint8_t *dy, uint8_t *du, uint8_t *dv) {
    xrgb_to_i420_rect(src, stride, w, h, 0, 0, w, h, dy, du, dv);
}
