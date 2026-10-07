#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host-endian words in GX2Surface ABI order. Returns zero for invalid input. */
int gx2_surface_layout(uint32_t words[29]);
uint32_t gx2_format_bits(uint32_t format);
/* pitch, height, depth, tile mode, bits/block, alignment, size, slice size */
int gx2_mip_layout(const uint32_t words[29], uint32_t level, uint32_t out[8]);
/* Transfer one slice between tiled storage and the CPU aperture. */
int gx2_aperture_copy(const uint32_t words[29], uint32_t level, uint32_t slice,
                      uint8_t *linear, uint32_t linear_size, uint8_t *tiled,
                      uint32_t tiled_size, int write_back);
int gx2_surface_read(const uint32_t words[29], uint32_t level, uint32_t slice,
                      uint8_t *linear, uint32_t linear_size, const uint8_t *tiled, uint32_t tiled_size);
#ifdef __cplusplus
}
#endif
