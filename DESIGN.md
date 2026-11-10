# Design

Design direction for the on-screen dashboard, recorded so every future
change starts from the same place (antislop audit 001, findings 2, 4, and
8). The dashboard is the only UI in the repo: the HTTP server exposes the
API and the static piece proxy, with no web frontend.

## Direction

Modern minimalist, clean, professional. Brand soul: comfort and ease.
The screen is read from a couch: calm surfaces, a uniform job list, no
motion. Dials: ENERGY 1 / RHYTHM 1 / MOTION 1.

## Typeface

Monospace bitmap font: DejaVu Sans Mono at size 15, rendered into 12x21
px cells by `tools/gen_font.py`. Reason: the fixed-cell bitmap renderer
keeps the UI layer simple and predictable on console hardware, and the
audience is technical. Comfort is delivered by size and spacing, not by
the face: the percent, speed, and eta figures render at full size
(24x42 px cells), while metadata stays at the small scale.

## Palette

Own neutrals on a PS4-adjacent blue-black, one teal accent, and three
status hues. Values live in `src/ui.c`; every text/surface pair used by
the dashboard passes WCAG AA (computed, worst pair 4.69:1).

| Token        | Value     | Role                            |
|--------------|-----------|---------------------------------|
| `COL_BG`     | `#0B1220` | canvas, darkest surface         |
| `COL_PANEL`  | `#111A2C` | card surface                    |
| `COL_PANEL2` | `#16233A` | status strip, selected card     |
| `COL_TRACK`  | `#1B2C46` | progress bar groove             |
| `COL_BORDER` | `#2E4160` | quiet blue boundary             |
| `TXT_TEXT`   | `#EAF0F8` | soft cool white, primary text   |
| `TXT_DIM`    | `#9AACC4` | blue-tinted gray, secondary     |
| `TXT_ACCENT` | `#39D2C0` | teal, clear of the status hues  |
| `TXT_GREEN`  | `#3FB950` | completed                       |
| `TXT_RED`    | `#F85149` | failed                          |
| `TXT_YELLOW` | `#D29922` | paused / waiting                |

Contrast (WCAG 2.x, computed): all 14 text/surface pairs the dashboard
uses pass AA (>= 4.5:1). Worst: `TXT_RED` on `COL_PANEL2` at 4.69:1.
Strongest: `TXT_TEXT` on `COL_BG` at 16.33:1.

## Rules of thumb for changes

- Keep surfaces calm and the structure driven by content: status strip,
  cards, log, footer.
- Every figure on screen must be real data with an honest `n/a` fallback.
- Text/surface pairs must pass WCAG AA before shipping; recompute when
  touching either side.
- No em dashes in product text; plain punctuation only.
