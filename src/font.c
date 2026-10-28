/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "font.h"
#include "font_data.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define FONT_SCALE_DEFAULT 2

static SDL_Texture* s_atlas = NULL;
static int s_atlas_w = 0;
static int s_atlas_h = 0;

bool font_init(SDL_Renderer* renderer)
{
    const int count = FONT_LAST - FONT_FIRST + 1;
    const int w = count * FONT_CELL_W;
    const int h = FONT_CELL_H;
    uint32_t* pixels;
    int i, y, x;
    SDL_Surface* surface;
    SDL_Texture* texture;

    if (s_atlas) {
        return true;
    }

    pixels = (uint32_t*)malloc((size_t)w * h * sizeof(uint32_t));
    if (!pixels) {
        return false;
    }

    for (i = 0; i < count; ++i) {
        for (y = 0; y < FONT_CELL_H; ++y) {
            unsigned short row = font_glyphs[i][y];
            for (x = 0; x < FONT_CELL_W; ++x) {
                int on = (row >> (15 - x)) & 1;
                pixels[(size_t)y * w + (size_t)i * FONT_CELL_W + (size_t)x] =
                    on ? 0xFFFFFFFFu : 0x00000000u;
            }
        }
    }

    surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                             SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        free(pixels);
        return false;
    }
    memcpy(surface->pixels, pixels, (size_t)w * h * sizeof(uint32_t));
    free(pixels);

    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) {
        return false;
    }

    s_atlas = texture;
    s_atlas_w = w;
    s_atlas_h = h;
    return true;
}

void font_fini(void)
{
    if (s_atlas) {
        SDL_DestroyTexture(s_atlas);
        s_atlas = NULL;
    }
}

int font_advance(int scale)
{
    return FONT_CELL_W * scale;
}

int font_height(int scale)
{
    return FONT_CELL_H * scale;
}

int font_draw(SDL_Renderer* renderer, int x, int y, int scale,
              uint32_t color, const char* text)
{
    if (!s_atlas || !text) {
        return x;
    }
    if (scale <= 0) {
        scale = FONT_SCALE_DEFAULT;
    }

    SDL_SetTextureColorMod(s_atlas, (Uint8)((color >> 16) & 0xFF),
                           (Uint8)((color >> 8) & 0xFF), (Uint8)(color & 0xFF));
    SDL_SetTextureAlphaMod(s_atlas, 255);

    for (; *text; ++text) {
        unsigned char c = (unsigned char)*text;
        SDL_Rect src;
        SDL_Rect dst;

        if (c < FONT_FIRST || c > FONT_LAST) {
            c = '?';
        }
        src.x = (c - FONT_FIRST) * FONT_CELL_W;
        src.y = 0;
        src.w = FONT_CELL_W;
        src.h = FONT_CELL_H;

        dst.x = x;
        dst.y = y;
        dst.w = FONT_CELL_W * scale;
        dst.h = FONT_CELL_H * scale;

        SDL_RenderCopy(renderer, s_atlas, &src, &dst);
        x += dst.w;
    }
    return x;
}

int font_drawf(SDL_Renderer* renderer, int x, int y, int scale,
               uint32_t color, const char* fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    return font_draw(renderer, x, y, scale, color, buf);
}

void font_fit(char* text, size_t textsz, int max_px, int scale)
{
    size_t max_chars;
    size_t len;

    if (!text || textsz == 0) {
        return;
    }
    max_chars = (size_t)(max_px / font_advance(scale));
    len = strlen(text);
    if (len <= max_chars) {
        return;
    }
    if (max_chars <= 2) {
        text[0] = '.';
        text[1] = '\0';
        return;
    }
    snprintf(text + max_chars - 2, textsz - max_chars + 2, "..");
}
