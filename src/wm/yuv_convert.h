#ifndef YUV_CONVERT_H
#define YUV_CONVERT_H

#include <stdint.h>

/* XRGB8888 (bytes B,G,R,X en memoria, como DRM_FORMAT_XRGB8888 en little-endian) -> I420 de rango
 * limitado (Y 16-235, UV 16-240) con BT.601: lo mismo que producía wf-recorder con
 * «-F scale=out_range=limited». w y h deben ser pares. Los planos son contiguos: Y w*h, U y V (w/2)*(h/2). */
void xrgb_to_i420(const uint8_t *src, int stride, int w, int h, uint8_t *y, uint8_t *u, uint8_t *v);

/* Igual, pero solo el rectángulo [x0,x1) x [y0,y1) (coordenadas pares, dentro de w x h): para actualizar solo
 * la zona dañada de la pantalla sobre un fotograma I420 ya completo. */
void xrgb_to_i420_rect(const uint8_t *src, int stride, int w, int h, int x0, int y0, int x1, int y1,
                       uint8_t *y, uint8_t *u, uint8_t *v);

#endif
