/**
 * config.h - Configuration for 3D Jelly
 * Talks directly to Jellyfin — no proxy needed.
 */

#pragma once

#define CONFIG_PATH "sdmc:/3ds/3djelly/config.ini"

typedef enum {
    RES_240P = 0,   /* 400x240 — native 3DS top screen */
    RES_144P = 1,   /* 256x144 — smaller, faster buffering */
} ResolutionMode;

typedef struct {
    /* Jellyfin server (direct — no proxy) */
    char jellyfin_host[256];   /* e.g. 192.168.1.50 or myserver.com */
    int  jellyfin_port;        /* default 8096 */
    char server_name[128];     /* display name from discovery */

    /* Auth (cached after login) */
    char token[256];
    char user_id[64];
    char username[64];

    /* Playback */
    ResolutionMode resolution_mode;
    int  audio_volume;         /* 0-100 */

    /* UI */
    int  show_thumbnails;      /* 1=yes, 0=no */

    /* State */
    int  setup_complete;       /* 1 after first successful login */
} Config;

Config *config_load(const char *path);
Config *config_create_default(void);
int     config_save(Config *config, const char *path);
void    config_free(Config *config);