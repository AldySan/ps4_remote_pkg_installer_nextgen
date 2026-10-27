/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Bitmap font rendering: a white glyph atlas texture built once at init,
 * tinted per draw call via SDL_SetTextureColorMod.
 */
#pragma once

#include <SDL2/SDL.h>

#include <stdbool.h>
#include <stdint.h>

/* Builds the atlas texture for the renderer. */
bool font_init(SDL_Renderer* renderer);

/* Frees the atlas. */
void font_fini(void);

/* Draws text at (x, y) with the given scale and color (0xRRGGBB).
 * Returns the x position after the last glyph. */
int font_draw(SDL_Renderer* renderer, int x, int y, int scale,
              uint32_t color, const char* text);

/* printf-style variant. Returns the x after the last glyph. */
int font_drawf(SDL_Renderer* renderer, int x, int y, int scale,
               uint32_t color, const char* fmt, ...);

/* Glyph advance (cell width * scale). */
int font_advance(int scale);

/* Glyph height (cell height * scale). */
int font_height(int scale);

/* Truncates text to fit max_px at the given scale (appends ".." when cut). */
void font_fit(char* text, size_t textsz, int max_px, int scale);
