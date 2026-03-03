/**
 * api.h - Direct Jellyfin API client for 3D Jelly
 *
 * Talks straight to Jellyfin — no proxy server needed.
 * Server discovery uses UDP broadcast to port 7359 (official Jellyfin protocol).
 * All other calls use Jellyfin's REST API with MediaBrowser auth header.
 */

#pragma once
#include <stdint.h>
#include "config.h"

/* ── Discovery ───────────────────────────────────────────────────────────── */

#define JELLYFIN_DISCOVERY_PORT  7359
#define MAX_DISCOVERED_SERVERS   8

typedef struct {
    char name[128];     /* Server display name, e.g. "My Jellyfin" */
    char address[256];  /* Full URL, e.g. "http://192.168.1.50:8096" */
    char id[64];        /* Server UUID */
} DiscoveredServer;

typedef struct {
    DiscoveredServer servers[MAX_DISCOVERED_SERVERS];
    int count;
} ServerDiscoveryResult;

/**
 * Broadcast "Who is JellyfinServer?" on UDP port 7359.
 * Collects responses for ~1.5 seconds then returns.
 * This is the official Jellyfin LAN discovery protocol.
 */
int api_discover_servers(ServerDiscoveryResult *out);

/* ── Item structures ─────────────────────────────────────────────────────── */

#define ITEM_ID_LEN    64
#define ITEM_NAME_LEN  128
#define ITEM_TYPE_LEN  32
#define THUMB_URL_LEN  256

typedef struct {
    char     id[ITEM_ID_LEN];
    char     name[ITEM_NAME_LEN];
    char     type[ITEM_TYPE_LEN];
    char     overview[256];
    char     thumb_url[THUMB_URL_LEN];
    int      year;
    uint64_t runtime_ticks;
    uint64_t resume_ticks;
    int      played;
    float    community_rating;
} Item;

typedef struct {
    Item *items;
    int   count;
    int   total_count;
    int   start;
    int   limit;
} ItemList;

typedef struct {
    char id[ITEM_ID_LEN];
    char name[ITEM_NAME_LEN];
    char type[ITEM_TYPE_LEN];
    char thumb_url[THUMB_URL_LEN];
} Library;

typedef struct {
    Library *items;
    int      count;
} ApiLibraryList;

typedef struct {
    char token[256];
    char user_id[64];
    char username[64];
    char server_name[128];
} ApiAuthResponse;

typedef struct {
    Item base;
    char genres[3][64];
    int  genre_count;
    char cast[5][64];
    int  cast_count;
    int  episode_number;
    int  season_number;
    char series_name[ITEM_NAME_LEN];
} ApiItemDetail;

/* ── Browse stack ────────────────────────────────────────────────────────── */
#define BROWSE_STACK_MAX 8

typedef struct {
    /* Jellyfin server (direct) */
    char base_url[280];   /* http://host:port */
    char host[256];
    int  port;

    /* Browse navigation */
    char browse_stack[BROWSE_STACK_MAX][ITEM_ID_LEN];
    int  browse_stack_depth;
    char current_parent_id[ITEM_ID_LEN];

    /* HTTP buffer */
    char  *http_buf;
    size_t http_buf_size;
} ApiContext;

/* ── Functions ───────────────────────────────────────────────────────────── */

ApiContext *api_init(const char *host, int port);
void        api_free(ApiContext *ctx);
void        api_set_server(ApiContext *ctx, const char *host, int port);

int api_authenticate(ApiContext *ctx, const char *username,
                     const char *password, ApiAuthResponse *out);

int api_get_libraries(ApiContext *ctx, const char *user_id,
                      const char *token, ApiLibraryList *out);

int api_get_items(ApiContext *ctx, const char *user_id, const char *token,
                  int start, int limit, ItemList *out);

int api_get_item_detail(ApiContext *ctx, const char *user_id,
                        const char *item_id, const char *token,
                        ApiItemDetail *out);

int api_get_resume_items(ApiContext *ctx, const char *user_id,
                         const char *token, ItemList **out);

/**
 * Build a direct HLS stream URL for Jellyfin.
 * No transcoding proxy needed — Jellyfin handles the transcode internally.
 */
void api_build_stream_url(ApiContext *ctx, const char *item_id,
                          const char *token, int width, int height,
                          char *out, size_t out_size);

void api_set_browse_parent(ApiContext *ctx, const char *parent_id);
int  api_can_go_back(ApiContext *ctx);
void api_go_back(ApiContext *ctx);

int api_report_playback_start(ApiContext *ctx, const char *item_id,
                              const char *user_id, const char *token);
int api_report_playback_progress(ApiContext *ctx, const char *item_id,
                                 uint64_t pos_ticks, int is_paused,
                                 const char *user_id, const char *token);
int api_report_playback_stop(ApiContext *ctx, const char *item_id,
                             uint64_t pos_ticks,
                             const char *user_id, const char *token);

void item_list_free(ItemList *list);