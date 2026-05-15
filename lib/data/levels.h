#pragma once

typedef struct {
    const char* name;
    const char* data;
    const char* id; /* stable identifier for persistence */
} EmbeddedLevel;

extern const EmbeddedLevel OFFICIAL_LEVELS[];
extern const int OFFICIAL_LEVEL_COUNT;
