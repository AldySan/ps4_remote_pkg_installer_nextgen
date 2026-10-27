/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Live dashboard: status strip, transfer cards with per-item controls,
 * activity log. Dark tech theme, controller navigation.
 */
#pragma once

#include <SDL2/SDL.h>

#include <stdbool.h>

bool ui_init(SDL_Renderer* renderer);
void ui_fini(void);

/* Controller input (SDL_ControllerButtonEvent values). */
void ui_handle_button(int button);

/* Draws one dashboard frame. */
void ui_frame(SDL_Renderer* renderer);
