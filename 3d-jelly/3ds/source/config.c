/**
 * config.c - INI-based configuration for 3D Jelly
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "config.h"

Config *config_create_default(void) {
    Config *c = (Config *)calloc(1, sizeof(Config));
    if (!c) return NULL;

    strncpy(c->jellyfin_host, "192.168.1.100", sizeof(c->jellyfin_host) - 1);
    c->jellyfin_port    = 8096;
    c->resolution_mode  = RES_240P;
    c->audio_volume     = 80;
    c->show_thumbnails  = 1;
    c->setup_complete   = 0;

    return c;
}

Config *config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    Config *c = config_create_default();
    if (!c) { fclose(f); return NULL; }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if      (strcmp(key, "jellyfin_host") == 0)
            strncpy(c->jellyfin_host, val, sizeof(c->jellyfin_host) - 1);
        else if (strcmp(key, "jellyfin_port") == 0)
            c->jellyfin_port = atoi(val);
        else if (strcmp(key, "server_name") == 0)
            strncpy(c->server_name, val, sizeof(c->server_name) - 1);
        else if (strcmp(key, "token") == 0)
            strncpy(c->token, val, sizeof(c->token) - 1);
        else if (strcmp(key, "user_id") == 0)
            strncpy(c->user_id, val, sizeof(c->user_id) - 1);
        else if (strcmp(key, "username") == 0)
            strncpy(c->username, val, sizeof(c->username) - 1);
        else if (strcmp(key, "resolution") == 0)
            c->resolution_mode = (atoi(val) == 1) ? RES_144P : RES_240P;
        else if (strcmp(key, "volume") == 0)
            c->audio_volume = atoi(val);
        else if (strcmp(key, "show_thumbnails") == 0)
            c->show_thumbnails = atoi(val);
        else if (strcmp(key, "setup_complete") == 0)
            c->setup_complete = atoi(val);
    }

    fclose(f);
    return c;
}

int config_save(Config *c, const char *path) {
    mkdir("sdmc:/3ds/", 0777);
    mkdir("sdmc:/3ds/3djelly/", 0777);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# 3D Jelly Configuration\n");
    fprintf(f, "# Do not edit while 3D Jelly is running\n\n");

    fprintf(f, "[server]\n");
    fprintf(f, "jellyfin_host=%s\n", c->jellyfin_host);
    fprintf(f, "jellyfin_port=%d\n", c->jellyfin_port);
    fprintf(f, "server_name=%s\n",   c->server_name);

    fprintf(f, "\n[auth]\n");
    fprintf(f, "token=%s\n",    c->token);
    fprintf(f, "user_id=%s\n",  c->user_id);
    fprintf(f, "username=%s\n", c->username);

    fprintf(f, "\n[playback]\n");
    fprintf(f, "resolution=%d\n",      (int)c->resolution_mode);
    fprintf(f, "volume=%d\n",          c->audio_volume);
    fprintf(f, "show_thumbnails=%d\n", c->show_thumbnails);

    fprintf(f, "\n[state]\n");
    fprintf(f, "setup_complete=%d\n", c->setup_complete);

    fclose(f);
    return 0;
}

void config_free(Config *c) {
    free(c);
}