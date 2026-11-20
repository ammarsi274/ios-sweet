#ifndef DRGLES_RT_MAP_H
#define DRGLES_RT_MAP_H
#include <stddef.h>
#include <stdint.h>

/*
 * What texel (x, y) of a buffer that frame k drew holds: 0 right, 1 zero, 2 clear colour (0x33 bytes),
 * 3 the pattern of another frame (*dk = that frame mod 256), 4 a pattern texel of another position
 * (*dx, *dy = its offset, frame k assumed), 5 anything else.
 */
int rt_class(const uint8_t *t, int x, int y, int k, int bgra, int *dk, int *dx, int *dy);

/*
 * Logs (each line starts with tag) the number of right texels of the w x h buffer m (rows bpr bytes
 * apart) as frame k drew them, the classes of the wrong ones, a 64x64-block map and the right column
 * runs per band of rows (at most 64 bands). Returns the number of right texels.
 */
long rt_report(const char *tag, const uint8_t *m, size_t bpr, int w, int h, int k, int bgra);

#endif
