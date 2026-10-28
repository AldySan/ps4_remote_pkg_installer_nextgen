/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * PKG icon (icon0.png) decoding for the dashboard, backed by stb_image.
 */
#pragma once

#include <SDL2/SDL.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Decodes the PNG at `path` into an SDL texture (alpha-blended).
 * Returns NULL when the file is missing or not a decodable image.
 * The caller owns the texture (SDL_DestroyTexture).
 */
SDL_Texture* icon_load(SDL_Renderer* renderer, const char* path,
                       int* out_w, int* out_h);
