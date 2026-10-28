/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Loads a pkg's icon0.png into an alpha-blended SDL texture. The file is
 * read into memory first (the app's own I/O paths are known to work on
 * console), then decoded by stb_image from the in-memory buffer.
 */
#include "icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stb_image.h>

SDL_Texture* icon_load(SDL_Renderer* renderer, const char* path,
                       int* out_w, int* out_h)
{
    FILE* f;
    long file_size;
    unsigned char* file_buf = NULL;
    unsigned char* rgba = NULL;
    SDL_Texture* texture = NULL;
    int w = 0;
    int h = 0;
    int n = 0;
    uint32_t* pixels = NULL;
    int i;

    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) <= 0 ||
        file_size > 8 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    file_buf = (unsigned char*)malloc((size_t)file_size);
    if (!file_buf) {
        fclose(f);
        return NULL;
    }
    if (fread(file_buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f);
        free(file_buf);
        return NULL;
    }
    fclose(f);

    rgba = stbi_load_from_memory(file_buf, (int)file_size, &w, &h, &n, 4);
    free(file_buf);
    if (!rgba || w <= 0 || h <= 0) {
        free(rgba);
        return NULL;
    }

    /* RGBA byte order (stb) -> ARGB8888 pixel words (SDL). */
    pixels = (uint32_t*)malloc((size_t)w * h * sizeof(uint32_t));
    if (pixels) {
        for (i = 0; i < w * h; ++i) {
            pixels[i] = ((uint32_t)rgba[i * 4 + 3] << 24) |
                        ((uint32_t)rgba[i * 4 + 2] << 16) |
                        ((uint32_t)rgba[i * 4 + 1] << 8) |
                        (uint32_t)rgba[i * 4 + 0];
        }
        {
            SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
                0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surface) {
                memcpy(surface->pixels, pixels, (size_t)w * h * sizeof(uint32_t));
                texture = SDL_CreateTextureFromSurface(renderer, surface);
                SDL_FreeSurface(surface);
            }
        }
        free(pixels);
    }
    free(rgba);

    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        if (out_w) {
            *out_w = w;
        }
        if (out_h) {
            *out_h = h;
        }
    }
    return texture;
}
