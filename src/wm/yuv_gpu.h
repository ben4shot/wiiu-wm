#ifndef YUV_GPU_H
#define YUV_GPU_H

#include <stdbool.h>
#include <stdint.h>

/* Conversión XRGB -> I420 (rango limitado BT.601) en la GPU, con lectura de solo 0,6 MB en lugar de 1,6 MB.
 *
 * Un pase de fragmentos dibuja en una textura RGBA8 de 214 x 720 texels donde cada texel empaqueta 4 bytes
 * consecutivos del plano: las filas 0..479 son la luma (4 muestras Y por texel, 856 bytes por fila con 2 de
 * relleno) y las filas 480..719 la croma (U en los texels 0..106 y V en los 107..213, 428 bytes cada una).
 * La CPU solo reordena filas (856 -> 854 / 428 -> 427).
 *
 * Requiere un contexto GLES2/3 actual. Pantalla fija de 854 x 480. */

typedef struct yuv_gpu yuv_gpu;

yuv_gpu *yuv_gpu_create(void);
void yuv_gpu_destroy(yuv_gpu *g);

/* Convierte la textura `tex` (GL_TEXTURE_2D o GL_TEXTURE_EXTERNAL_OES con target `target`) y copia a los planos
 * contiguos y (854x480), u y v (427x240) las filas de luma [y0, y1) y sus filas de croma (y0, y1 pares). */
bool yuv_gpu_convert(yuv_gpu *g, unsigned tex, unsigned target, int y0, int y1,
                     uint8_t *y, uint8_t *u, uint8_t *v);

#endif
