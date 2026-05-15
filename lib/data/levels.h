#pragma once

typedef struct {
    const char* name;
    const char* data;
} EmbeddedLevel;

extern const EmbeddedLevel OFFICIAL_LEVELS[];
extern const int OFFICIAL_LEVEL_COUNT;
