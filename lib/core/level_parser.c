#include "level_parser.h"

#include <furi.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Sort helpers ───────────────────────────────────────────────── */

/* Simple insertion sort — fast for nearly-sorted level data */
static void sort_objects(LvlObject* arr, int n) {
    for(int i = 1; i < n; i++) {
        LvlObject key = arr[i];
        int j = i - 1;
        while(j >= 0 && arr[j].gx > key.gx) {
            arr[j+1] = arr[j];
            j--;
        }
        arr[j+1] = key;
    }
}

/* ─── Music directive parsing ────────────────────────────────────────
   Level files may declare background music with a Flipper Music Format
   style note list:
     MUSIC BPM=130 DURATION=8 OCTAVE=5
     NOTE E6, P, E, B, 4P, E, A, G, A, ...
     NOTE D, B, 4P, D, A, G, A, D, F#, ...
   NOTE lines are concatenated (comma-joined) into one note list so a long
   tune can be split across several short lines. */

static void parse_music_directive(const char* body, Level* lvl) {
    const char* p;
    if((p = strstr(body, "BPM=")))      lvl->music_bpm      = (int16_t)atoi(p + 4);
    if((p = strstr(body, "DURATION="))) lvl->music_duration = (int16_t)atoi(p + 9);
    if((p = strstr(body, "OCTAVE=")))   lvl->music_octave   = (int16_t)atoi(p + 7);
}

static void append_music_notes(Level* lvl, const char* chunk) {
    size_t cap = sizeof(lvl->music_notes);
    size_t used = strlen(lvl->music_notes);
    if(used > 0 && used + 1 < cap) {
        lvl->music_notes[used++] = ',';
        lvl->music_notes[used]   = '\0';
    }
    size_t remain = (used + 1 < cap) ? (cap - used - 1) : 0;
    size_t clen = strlen(chunk);
    if(clen > remain) clen = remain;
    memcpy(lvl->music_notes + used, chunk, clen);
    lvl->music_notes[used + clen] = '\0';
}

/* ─── Level Parser ───────────────────────────────────────────────── */

bool parse_level(const char* path, Level* lvl) {
    memset(lvl, 0, sizeof(Level));
    lvl->speed       = SCROLL_SPEED;
    lvl->gravity_pct = 100;
    lvl->bg_style    = '0';
    lvl->length      = 2000;
    lvl->music_bpm      = 0;
    lvl->music_duration = 8;
    lvl->music_octave   = 5;
    strncpy(lvl->name, "Unnamed", sizeof(lvl->name) - 1);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File*    file    = storage_file_alloc(storage);

    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    char   line[128];
    int    idx = 0;
    char   ch;

    while(storage_file_read(file, &ch, 1) == 1) {
        if(ch == '\n' || ch == '\r') {
            if(idx == 0) continue;
            line[idx] = '\0';
            idx = 0;
            if(line[0] == '#') continue;

            if     (strncmp(line, "NAME ",    5) == 0) strncpy(lvl->name, line+5, 63);
            else if(strncmp(line, "SPEED ",   6) == 0) lvl->speed       = (int16_t)atoi(line+6);
            else if(strncmp(line, "GRAVITY ", 8) == 0) lvl->gravity_pct = (int16_t)atoi(line+8);
            else if(strncmp(line, "BG ",      3) == 0) lvl->bg_style    = line[3];
            else if(strncmp(line, "DIFICULTY ", 10) == 0) {
                strncpy(lvl->difficulty, line + 10, sizeof(lvl->difficulty)-1);
                lvl->difficulty[sizeof(lvl->difficulty)-1] = '\0';
            }
            else if(strncmp(line, "LENGTH ",  7) == 0) lvl->length      = atoi(line+7);
            else if(strncmp(line, "MUSIC ",   6) == 0) parse_music_directive(line + 6, lvl);
            else if(strncmp(line, "NOTE ",    5) == 0) append_music_notes(lvl, line + 5);
            else if(strncmp(line, "OBJ ",     4) == 0 && lvl->obj_count < MAX_OBJECTS) {
                char ts[16] = {0};
                int  gx = 0, gy = 0, rot = 0;
                int scanned = sscanf(line+4, "%15s %d %d %d", ts, &gx, &gy, &rot);
                if(scanned < 3) continue;
                ObjType t;
                if     (strcmp(ts,"BLOCK") == 0)      t = OBJ_BLOCK;
                else if(strcmp(ts,"SPIKE") == 0)      t = OBJ_SPIKE;
                else if(strcmp(ts,"MINI_SPIKE") == 0) t = OBJ_MINI_SPIKE;
                else if(strcmp(ts,"MINI_BLOCK") == 0) t = OBJ_MINI_BLOCK;
                else if(strcmp(ts,"JUMPER") == 0)     t = OBJ_JUMPER;
                else if(strcmp(ts,"SPHERE") == 0)     t = OBJ_SPHERE;
                else continue;
                int rot_idx = 0;
                if(scanned >= 4) {
                    if(rot % 90 == 0) {
                        int r = ((rot % 360) + 360) % 360;
                        rot_idx = (r / 90) & 3;
                    } else {
                        rot_idx = ((rot % 4) + 4) % 4;
                    }
                }
                lvl->objects[lvl->obj_count++] = (LvlObject){t, (int16_t)gx, (int16_t)gy, (int8_t)rot_idx};
            }
            else if(strncmp(line, "DEC ", 4) == 0 && lvl->dec_count < MAX_DECORATIONS) {
                char ts[16] = {0};
                int  x = 0, y = 0;
                sscanf(line+4, "%15s %d %d", ts, &x, &y);
                DecType t;
                if     (strcmp(ts,"STAR")   == 0) t = DEC_STAR;
                else if(strcmp(ts,"CLOUD")  == 0) t = DEC_CLOUD;
                else if(strcmp(ts,"PILLAR") == 0) t = DEC_PILLAR;
                else continue;
                lvl->decorations[lvl->dec_count++] = (Decoration){t, (int16_t)x, (int8_t)y};
            }
        } else if(idx < (int)sizeof(line) - 1) {
            line[idx++] = ch;
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    /* Sort objects by GX so we can use a sliding window during gameplay */
    sort_objects(lvl->objects, lvl->obj_count);
    return true;
}

bool parse_level_from_text(const char* text, Level* lvl) {
    memset(lvl, 0, sizeof(Level));
    lvl->speed       = SCROLL_SPEED;
    lvl->gravity_pct = 100;
    lvl->bg_style    = '0';
    lvl->length      = 2000;
    lvl->music_bpm      = 0;
    lvl->music_duration = 8;
    lvl->music_octave   = 5;
    strncpy(lvl->name, "Unnamed", sizeof(lvl->name) - 1);

    char line[128];
    int idx = 0;
    for(const char* p = text; ; p++) {
        char ch = *p;
        if(ch == '\n' || ch == '\r' || ch == '\0') {
            if(idx > 0) {
                line[idx] = '\0';
                idx = 0;
                if(line[0] != '#') {
                    if(strncmp(line, "NAME ", 5) == 0) strncpy(lvl->name, line + 5, 63);
                    else if(strncmp(line, "DIFICULTY ", 10) == 0) {
                        strncpy(lvl->difficulty, line + 10, sizeof(lvl->difficulty)-1);
                        lvl->difficulty[sizeof(lvl->difficulty)-1] = '\0';
                    }
                    else if(strncmp(line, "SPEED ", 6) == 0) lvl->speed = (int16_t)atoi(line + 6);
                    else if(strncmp(line, "GRAVITY ", 8) == 0) lvl->gravity_pct = (int16_t)atoi(line + 8);
                    else if(strncmp(line, "BG ", 3) == 0) lvl->bg_style = line[3];
                    else if(strncmp(line, "LENGTH ", 7) == 0) lvl->length = atoi(line + 7);
                    else if(strncmp(line, "MUSIC ", 6) == 0) parse_music_directive(line + 6, lvl);
                    else if(strncmp(line, "NOTE ", 5) == 0) append_music_notes(lvl, line + 5);
                    else if(strncmp(line, "OBJ ", 4) == 0 && lvl->obj_count < MAX_OBJECTS) {
                        char ts[16] = {0};
                        int gx = 0, gy = 0, rot = 0;
                        int scanned = sscanf(line + 4, "%15s %d %d %d", ts, &gx, &gy, &rot);
                        if(scanned < 3) continue;
                        ObjType t;
                        if(strcmp(ts, "BLOCK") == 0) t = OBJ_BLOCK;
                        else if(strcmp(ts, "SPIKE") == 0) t = OBJ_SPIKE;
                        else if(strcmp(ts, "MINI_SPIKE") == 0) t = OBJ_MINI_SPIKE;
                        else if(strcmp(ts, "MINI_BLOCK") == 0) t = OBJ_MINI_BLOCK;
                        else if(strcmp(ts, "JUMPER") == 0) t = OBJ_JUMPER;
                        else if(strcmp(ts, "SPHERE") == 0) t = OBJ_SPHERE;
                        else continue;
                        int rot_idx = 0;
                        if(scanned >= 4) {
                            if(rot % 90 == 0) {
                                int r = ((rot % 360) + 360) % 360;
                                rot_idx = (r / 90) & 3;
                            } else {
                                rot_idx = ((rot % 4) + 4) % 4;
                            }
                        }
                        lvl->objects[lvl->obj_count++] = (LvlObject){t, (int16_t)gx, (int16_t)gy, (int8_t)rot_idx};
                    } else if(strncmp(line, "DEC ", 4) == 0 && lvl->dec_count < MAX_DECORATIONS) {
                        char ts[16] = {0};
                        int x = 0, y = 0;
                        sscanf(line + 4, "%15s %d %d", ts, &x, &y);
                        DecType t;
                        if(strcmp(ts, "STAR") == 0) t = DEC_STAR;
                        else if(strcmp(ts, "CLOUD") == 0) t = DEC_CLOUD;
                        else if(strcmp(ts, "PILLAR") == 0) t = DEC_PILLAR;
                        else continue;
                        lvl->decorations[lvl->dec_count++] = (Decoration){t, (int16_t)x, (int8_t)y};
                    }
                }
            }
            if(ch == '\0') break;
        } else if(idx < (int)sizeof(line) - 1) {
            line[idx++] = ch;
        }
    }

    sort_objects(lvl->objects, lvl->obj_count);
    return true;
}

/* ─── Level Discovery ────────────────────────────────────────────── */

int discover_levels(char files[][MAX_PATH_LEN], int max) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, LEVEL_DIR);
    File* dir   = storage_file_alloc(storage);
    int   count = 0;
    if(storage_dir_open(dir, LEVEL_DIR)) {
        FileInfo fi;
        char     name[MAX_FILENAME];
        while(count < max && storage_dir_read(dir, &fi, name, sizeof(name))) {
            if(!(fi.flags & FSF_DIRECTORY)) {
                int len = (int)strlen(name);
                if(len > 6 && strcmp(name + len - 6, ".gdlvl") == 0) {
                    snprintf(files[count], MAX_PATH_LEN, "%s/%s", LEVEL_DIR, name);
                    count++;
                }
            }
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    return count;
}

void ensure_app_storage_dirs(void) {
    const char* root_dir = "/ext/geoflip";
    const char* levels_dir = "" LEVEL_DIR;
    const char* player_dir = "/ext/geoflip/player";

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, root_dir);
    storage_simply_mkdir(storage, levels_dir);
    storage_simply_mkdir(storage, player_dir);
    furi_record_close(RECORD_STORAGE);
}
