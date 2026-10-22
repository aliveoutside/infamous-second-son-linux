// SPDX-License-Identifier: GPL-2.0-or-later
// The package profile (PlayGo chunk sizes, trophy IDs) prepared from the player's own game folder
// by scripts/content_profile.py, read at startup from the folder holding boot-linked.bin,
// so nothing from the game is compiled in. Also the import-name lookup (api_names.h, generated
// from shadPS4's aerolib at build time).
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int content_package_complete;
unsigned content_chunk_count, content_trophy_count;
uint64_t content_chunk_sizes[CONTENT_MAX_CHUNKS];
unsigned char content_trophy_ids[CONTENT_MAX_TROPHIES];
#include "api_names.h"

static int load_profile(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char key[16];
    int ok = 1;
    while (ok && fscanf(f, "%15s", key) == 1) {
        if (!strcmp(key, "complete")) ok = fscanf(f, "%d", &content_package_complete) == 1;
        else if (!strcmp(key, "chunks")) {
            unsigned long long size;
            while (content_chunk_count < CONTENT_MAX_CHUNKS && fscanf(f, " %llu", &size) == 1)
                content_chunk_sizes[content_chunk_count++] = size;
        } else if (!strcmp(key, "trophies")) {
            unsigned id;
            while (content_trophy_count < CONTENT_MAX_TROPHIES && fscanf(f, " %u", &id) == 1)
                content_trophy_ids[content_trophy_count++] = (unsigned char)id;
        } else ok = 0;
    }
    fclose(f);
    return ok && content_chunk_count && content_trophy_count ? 0 : -1;
}

int content_load(const char *dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/content_profile.txt", dir);
    if (load_profile(path)) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    return 0;
}

static int compare_nid(const void *key, const void *entry) {
    const char *nid = key, *other = ((const typeof(*api_names) *)entry)->nid;
    size_t length = strcspn(nid, "#");
    int order = strncmp(nid, other, length);
    return order ? order : -(unsigned char)other[length];
}
// identity is "NID#library#...": the name is looked up by its NID.
const char *runtime_api_name(const char *identity) {
    const typeof(*api_names) *found = bsearch(identity, api_names, sizeof(api_names) / sizeof(*api_names),
                                              sizeof(*api_names), compare_nid);
    return found ? found->api : "";
}
