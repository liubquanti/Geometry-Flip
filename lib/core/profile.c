#include "profile.h"

#include <furi.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../data/skins.h"
#include "../data/levels.h"

/* ─── Player profile (persistence) ───────────────────────────────── */
void save_player_profile(GeoApp* app) {
    const char* dir = "/ext/geoflip/player";
    const char* path = "/ext/geoflip/player/profile.txt";
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, dir);
    File* file = storage_file_alloc(storage);
    /* Try opening existing file for write first, otherwise create/truncate */
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_EXISTING)) {
        storage_file_close(file);
        storage_file_free(file);
        /* try create new */
        file = storage_file_alloc(storage);
        if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW)) {
            storage_file_free(file);
            furi_record_close(RECORD_STORAGE);
            return;
        }
    }
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)app->selected_skin);
    storage_file_write(file, buf, len);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

void load_player_profile(GeoApp* app) {
    const char* dir = "/ext/geoflip/player";
    const char* path = "/ext/geoflip/player/profile.txt";
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, dir);
    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        /* create default file */
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        save_player_profile(app);
        return;
    }
    char buf[32] = {0};
    int read = storage_file_read(file, buf, sizeof(buf)-1);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(read > 0) {
        int v = atoi(buf);
        if(v >= 0 && v < SKIN_COUNT) app->selected_skin = (int8_t)v;
    }
}

/* ─── Official progression persistence ───────────────────────────── */
void save_official_progress(GeoApp* app) {
    const char* dir = "/ext/geoflip/player";
    const char* path = "/ext/geoflip/player/ofprog.txt";
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, dir);
    File* file = storage_file_alloc(storage);
    /* Try opening existing file for write first, otherwise create/truncate */
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_EXISTING)) {
        storage_file_close(file);
        storage_file_free(file);
        file = storage_file_alloc(storage);
        if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW)) {
            storage_file_free(file);
            furi_record_close(RECORD_STORAGE);
            return;
        }
    }
    /* Serialize as lines: ID = value\n */
    for(int i = 0; i < OFFICIAL_LEVEL_COUNT; i++) {
        const char* id = OFFICIAL_LEVELS[i].id ? OFFICIAL_LEVELS[i].id : OFFICIAL_LEVELS[i].name;
        char line[64];
        int len = snprintf(line, sizeof(line), "%s = %d\n", id, app->official_prog ? (int)app->official_prog[i] : 0);
        if(len > 0) storage_file_write(file, line, len);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

void load_official_progress(GeoApp* app) {
    const char* dir = "/ext/geoflip/player";
    const char* path = "/ext/geoflip/player/ofprog.txt";
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, dir);
    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        /* create default (zeros) */
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        /* ensure default saved file exists */
        save_official_progress(app);
        return;
    }
    char buf[256] = {0};
    int read = storage_file_read(file, buf, sizeof(buf)-1);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(read <= 0) return;
    /* parse lines of form: ID = value  (ignore unknown IDs) */
    char* p = buf;
    while(p && *p) {
        char* nl = strchr(p, '\n');
        if(nl) *nl = '\0';
        char* l = p;
        /* trim leading whitespace */
        while(*l == ' ' || *l == '\r' || *l == '\t') l++;
        if(*l != '\0' && *l != '#') {
            char* eq = strchr(l, '=');
            if(eq) {
                *eq = '\0';
                char* key = l;
                char* valstr = eq + 1;
                while(*key == ' ' || *key == '\t') key++;
                char* kend = key + strlen(key) - 1;
                while(kend > key && (*kend == ' ' || *kend == '\t')) { *kend = '\0'; kend--; }
                while(*valstr == ' ' || *valstr == '\t') valstr++;
                int v = atoi(valstr);
                if(v < 0) v = 0;
                if(v > 100) v = 100;
                for(int i = 0; i < OFFICIAL_LEVEL_COUNT; i++) {
                    const char* id = OFFICIAL_LEVELS[i].id ? OFFICIAL_LEVELS[i].id : OFFICIAL_LEVELS[i].name;
                    if(id && strcmp(id, key) == 0) {
                        if(app->official_prog) app->official_prog[i] = (int8_t)v;
                        break;
                    }
                }
            }
        }
        if(!nl) break;
        p = nl + 1;
    }
    /* ensure any unspecified entries are zeroed */
    for(int i = 0; i < OFFICIAL_LEVEL_COUNT; i++) if(app->official_prog) {
        if(app->official_prog[i] < 0 || app->official_prog[i] > 100) app->official_prog[i] = 0;
    }
}
