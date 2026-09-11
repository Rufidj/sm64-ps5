/* hd_textures - the game's textures replaced by an sm64ex texture pack. */
#ifndef HD_TEXTURES_H
#define HD_TEXTURES_H

#include <stdbool.h>
#include <stdint.h>

/* Loads the index (hd_tools/hd_index.py) and looks for a pack in the game's
 * data folder. */
void hd_textures_init(void);

/* Whether both the index and a pack were found. */
bool hd_textures_available(void);

/* The pack's image for a texture the game is importing, given the texture's
 * own bytes: RGBA, 8 bits a channel, with its size, or NULL if the pack has
 * none. Freed with hd_texture_free. */
uint8_t *hd_texture_load(const uint8_t *data, uint32_t size, int *width, int *height);
void     hd_texture_free(uint8_t *pixels);

#endif
