/* Level file discovery and parsing: turns a .gdlvl file (or an embedded
   official-level text blob) into a populated Level struct. */
#pragma once

#include <stdbool.h>
#include "../geoflip.h"

/* Parse a level from a file on storage at `path`. Returns false if the
   file could not be opened; `lvl` is always reset to defaults first. */
bool parse_level(const char* path, Level* lvl);

/* Parse a level from an in-memory null-terminated text blob (used for the
   OFFICIAL_LEVELS embedded in lib/data/levels.c). */
bool parse_level_from_text(const char* text, Level* lvl);

/* Scan LEVEL_DIR for *.gdlvl files, filling `files` (up to `max` entries).
   Returns the number of levels found. Creates LEVEL_DIR if missing. */
int discover_levels(char files[][MAX_PATH_LEN], int max);

/* Ensure /ext/geoflip, /ext/geoflip/levels and /ext/geoflip/player exist. */
void ensure_app_storage_dirs(void);
