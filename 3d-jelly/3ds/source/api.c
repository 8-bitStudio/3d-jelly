/**
 * api.c - Direct Jellyfin API client
 *
 * Server discovery: UDP broadcast "Who is JellyfinServer?" → port 7359
 * All REST calls go straight to Jellyfin using MediaBrowser auth header.
 * No proxy server required.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "cJSON.h"
#include "api.h"
#include "config.h"

#define HTTP_BUF_SIZE  (256 * 1024)   /* 256 KB */
#define DEVICE_ID      "3djelly-3ds-001"
#define CLIENT_NAME    "3DJelly"
#define CLIENT_VERSION "1.0.0"
#define DEVICE_NAME    "Nintendo3DS"

/* ── Auth header builder ─────────────────────────────────────────────────── */

static void build_auth_header(const char *token, char *out, size_t out_size) {
    if (token && token[0]) {
        snprintf(out, out_size,
            "MediaBrowser Client=\"%s\", Device=\"%s\", "
            "DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"",
            CLIENT_NAME, DEVICE_NAME, DEVICE_ID, CLIENT_VERSION, token);
    } else {
        snprintf(out, out_size,
            "MediaBrowser Client=\"%s\", Device=\"%s\", "
            "DeviceId=\"%s\", Version=\"%s\"",
            CLIENT_NAME, DEVICE_NAME, DEVICE_ID, CLIENT_VERSION);
    }
}

/* ── HTTP helpers ────────────────────────────────────────────────────────── */

static int http_get(ApiContext *ctx, const char *url, const char *token,
                    char **out_buf, size_t *out_len) {
    httpcContext hctx;
    char auth_hdr[512];
    build_auth_header(token, auth_hdr, sizeof(auth_hdr));

    Result rc = httpcOpenContext(&hctx, HTTPC_METHOD_GET, url, 1);
    if (R_FAILED(rc)) return -1;

    httpcAddRequestHeaderField(&hctx, "Authorization", auth_hdr);
    httpcAddRequestHeaderField(&hctx, "Accept", "application/json");
    httpcAddRequestHeaderField(&hctx, "X-Emby-Authorization", auth_hdr);

    rc = httpcBeginRequest(&hctx);
    if (R_FAILED(rc)) { httpcCloseContext(&hctx); return -2; }

    u32 status = 0;
    httpcGetResponseStatusCode(&hctx, &status);
    if (status < 200 || status >= 300) {
        httpcCloseContext(&hctx);
        return -(int)status;
    }

    u32 bytes_read = 0;
    rc = httpcDownloadData(&hctx, (u8 *)ctx->http_buf, HTTP_BUF_SIZE - 1, &bytes_read);
    ctx->http_buf[bytes_read] = '\0';
    httpcCloseContext(&hctx);

    if (R_FAILED(rc) && rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)
        return -3;

    *out_buf = ctx->http_buf;
    *out_len = bytes_read;
    return 0;
}

static int http_post(ApiContext *ctx, const char *url, const char *token,
                     const char *body, char **out_buf, size_t *out_len) {
    httpcContext hctx;
    char auth_hdr[512];
    build_auth_header(token, auth_hdr, sizeof(auth_hdr));

    Result rc = httpcOpenContext(&hctx, HTTPC_METHOD_POST, url, 1);
    if (R_FAILED(rc)) return -1;

    httpcAddRequestHeaderField(&hctx, "Authorization", auth_hdr);
    httpcAddRequestHeaderField(&hctx, "X-Emby-Authorization", auth_hdr);
    httpcAddRequestHeaderField(&hctx, "Content-Type", "application/json");
    httpcAddRequestHeaderField(&hctx, "Accept", "application/json");

    if (body && body[0])
        httpcAddPostDataRaw(&hctx, (const u32 *)body, strlen(body));

    rc = httpcBeginRequest(&hctx);
    if (R_FAILED(rc)) { httpcCloseContext(&hctx); return -2; }

    u32 status = 0;
    httpcGetResponseStatusCode(&hctx, &status);
    if (status < 200 || status >= 300) {
        httpcCloseContext(&hctx);
        return -(int)status;
    }

    u32 bytes_read = 0;
    httpcDownloadData(&hctx, (u8 *)ctx->http_buf, HTTP_BUF_SIZE - 1, &bytes_read);
    ctx->http_buf[bytes_read] = '\0';
    httpcCloseContext(&hctx);

    *out_buf = ctx->http_buf;
    *out_len = bytes_read;
    return 0;
}

/* ── Server Discovery ────────────────────────────────────────────────────── */
/*
 * Official Jellyfin LAN discovery protocol:
 *   1. Send "Who is JellyfinServer?" as UDP broadcast to 255.255.255.255:7359
 *   2. Collect JSON responses for ~1.5 seconds
 *   3. Each response contains: { "Id": "...", "Name": "...", "Address": "http://..." }
 *
 * Reference: https://jellyfin.org/docs/general/networking/
 */
int api_discover_servers(ServerDiscoveryResult *out) {
    memset(out, 0, sizeof(*out));

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    /* Enable broadcast */
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    /* Set receive timeout: 1500ms */

    /* Bind to any port so we can receive responses */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port        = 0;  /* OS picks port */
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    /* Send broadcast discovery message */
    const char *msg = "Who is JellyfinServer?";
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest.sin_port        = htons(JELLYFIN_DISCOVERY_PORT);

    sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&dest, sizeof(dest));

    /* Collect responses */
    char buf[1024];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);


    fd_set fds;
    struct timeval tv;
    int attempts = 0;
    while (out->count < MAX_DISCOVERED_SERVERS && attempts < 30) {
        attempts++;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        tv.tv_sec  = 0;
        tv.tv_usec = 50000;  /* 50ms per attempt, 30 attempts = 1.5s max */
        int ready = select(sock + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0) break;  /* timeout or error */
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &from_len);
        if (n <= 0) break;
        buf[n] = '\0';
        cJSON *json = cJSON_Parse(buf);
        if (!json) continue;
        const char *id   = cJSON_GetStringValue(cJSON_GetObjectItem(json, "Id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(json, "Name"));
        const char *addr = cJSON_GetStringValue(cJSON_GetObjectItem(json, "Address"));
        if (id && name && addr) {
            DiscoveredServer *s = &out->servers[out->count];
            strncpy(s->id,      id,   sizeof(s->id) - 1);
            strncpy(s->name,    name, sizeof(s->name) - 1);
            strncpy(s->address, addr, sizeof(s->address) - 1);
            out->count++;
        }
        cJSON_Delete(json);
    }


    close(sock);
    return 0;
}

/* ── Init / Free ─────────────────────────────────────────────────────────── */

ApiContext *api_init(const char *host, int port) {
    ApiContext *ctx = (ApiContext *)calloc(1, sizeof(ApiContext));
    if (!ctx) return NULL;

    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    ctx->port = port;
    snprintf(ctx->base_url, sizeof(ctx->base_url), "http://%s:%d", host, port);

    ctx->http_buf = (char *)malloc(HTTP_BUF_SIZE);
    if (!ctx->http_buf) { free(ctx); return NULL; }
    ctx->http_buf_size = HTTP_BUF_SIZE;

    httpcInit(0);
    return ctx;
}

void api_free(ApiContext *ctx) {
    if (!ctx) return;
    free(ctx->http_buf);
    httpcExit();
    free(ctx);
}

void api_set_server(ApiContext *ctx, const char *host, int port) {
    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    ctx->port = port;
    snprintf(ctx->base_url, sizeof(ctx->base_url), "http://%s:%d", host, port);
}

int api_probe_server(ApiContext *ctx) {
    char url[512], *resp;
    size_t resp_len;

    /* Official Jellyfin endpoint for anonymous server metadata. */
    snprintf(url, sizeof(url), "%s/System/Info/Public", ctx->base_url);
    return http_get(ctx, url, "", &resp, &resp_len);
}

/* ── Authentication ──────────────────────────────────────────────────────── */
/*
 * POST /Users/AuthenticateByName
 * Body: { "Username": "...", "Pw": "..." }
 * Returns AccessToken + User object
 */
int api_authenticate(ApiContext *ctx, const char *username,
                     const char *password, ApiAuthResponse *out) {
    char url[512], *body = NULL, *resp;
    size_t resp_len;

    snprintf(url,  sizeof(url),  "%s/Users/AuthenticateByName", ctx->base_url);

    /* Build JSON via cJSON so usernames/passwords with quotes or backslashes
     * are escaped correctly (matches Jellyfin API contract for Username + Pw). */
    cJSON *req = cJSON_CreateObject();
    if (!req) return -12;
    cJSON_AddStringToObject(req, "Username", username ? username : "");
    cJSON_AddStringToObject(req, "Pw", password ? password : "");
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -13;

    /* Auth endpoint uses no token — just the MediaBrowser client header */
    int rc = http_post(ctx, url, "", body, &resp, &resp_len);
    free(body);
    if (rc != 0) return rc;

    cJSON *json = cJSON_Parse(resp);
    if (!json) return -10;

    const char *token   = cJSON_GetStringValue(cJSON_GetObjectItem(json, "AccessToken"));
    cJSON      *user    = cJSON_GetObjectItem(json, "User");
    const char *user_id = user ? cJSON_GetStringValue(cJSON_GetObjectItem(user, "Id"))   : NULL;
    const char *uname   = user ? cJSON_GetStringValue(cJSON_GetObjectItem(user, "Name")) : NULL;

    /* Also try to get server name from SessionInfo */
    cJSON      *sess     = cJSON_GetObjectItem(json, "SessionInfo");
    cJSON      *srv_item = cJSON_GetObjectItem(json, "ServerId");
    const char *srv_name = sess ? cJSON_GetStringValue(cJSON_GetObjectItem(sess, "ServerName")) : NULL;

    if (!token || !user_id) { cJSON_Delete(json); return -11; }

    memset(out, 0, sizeof(*out));
    strncpy(out->token,       token,              sizeof(out->token) - 1);
    strncpy(out->user_id,     user_id,            sizeof(out->user_id) - 1);
    strncpy(out->username,    uname ? uname : username, sizeof(out->username) - 1);
    strncpy(out->server_name, srv_name ? srv_name : ctx->host, sizeof(out->server_name) - 1);

    cJSON_Delete(json);
    return 0;
}

/* ── Libraries ───────────────────────────────────────────────────────────── */
/*
 * GET /Users/{userId}/Views
 * Returns library folders (Movies, TV Shows, Music, etc.)
 */
int api_get_libraries(ApiContext *ctx, const char *user_id,
                      const char *token, ApiLibraryList *out) {
    char url[512], *resp;
    size_t resp_len;

    snprintf(url, sizeof(url), "%s/Users/%s/Views", ctx->base_url, user_id);

    int rc = http_get(ctx, url, token, &resp, &resp_len);
    if (rc != 0) return rc;

    cJSON *json = cJSON_Parse(resp);
    if (!json) return -10;

    cJSON *items = cJSON_GetObjectItem(json, "Items");
    if (!cJSON_IsArray(items)) { cJSON_Delete(json); return -11; }

    int count = cJSON_GetArraySize(items);
    out->items = (Library *)calloc(count, sizeof(Library));
    out->count = 0;

    for (int i = 0; i < count; i++) {
        cJSON *lib = cJSON_GetArrayItem(items, i);
        Library *l = &out->items[out->count];

        const char *id       = cJSON_GetStringValue(cJSON_GetObjectItem(lib, "Id"));
        const char *name     = cJSON_GetStringValue(cJSON_GetObjectItem(lib, "Name"));
        const char *col_type = cJSON_GetStringValue(cJSON_GetObjectItem(lib, "CollectionType"));

        if (!id || !name) continue;
        strncpy(l->id,   id,   sizeof(l->id) - 1);
        strncpy(l->name, name, sizeof(l->name) - 1);
        strncpy(l->type, col_type ? col_type : "unknown", sizeof(l->type) - 1);

        /* Build thumbnail URL */
        snprintf(l->thumb_url, sizeof(l->thumb_url),
                 "%s/Items/%s/Images/Primary?maxWidth=128&maxHeight=128&quality=70",
                 ctx->base_url, id);

        out->count++;
    }

    cJSON_Delete(json);
    return 0;
}

/* ── Items ───────────────────────────────────────────────────────────────── */

static void parse_item(cJSON *j, Item *out, const char *base_url) {
    const char *id    = cJSON_GetStringValue(cJSON_GetObjectItem(j, "Id"));
    const char *name  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "Name"));
    const char *type  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "Type"));
    const char *ov    = cJSON_GetStringValue(cJSON_GetObjectItem(j, "Overview"));

    cJSON *runtime = cJSON_GetObjectItem(j, "RunTimeTicks");
    cJSON *year    = cJSON_GetObjectItem(j, "ProductionYear");
    cJSON *rating  = cJSON_GetObjectItem(j, "CommunityRating");

    /* UserData for played + resume position */
    cJSON *ud      = cJSON_GetObjectItem(j, "UserData");
    cJSON *played  = ud ? cJSON_GetObjectItem(ud, "Played") : NULL;
    cJSON *resume  = ud ? cJSON_GetObjectItem(ud, "PlaybackPositionTicks") : NULL;

    strncpy(out->id,       id   ? id   : "", sizeof(out->id) - 1);
    strncpy(out->name,     name ? name : "", sizeof(out->name) - 1);
    strncpy(out->type,     type ? type : "", sizeof(out->type) - 1);
    strncpy(out->overview, ov   ? ov   : "", sizeof(out->overview) - 1);

    out->runtime_ticks    = runtime ? (uint64_t)runtime->valuedouble : 0;
    out->resume_ticks     = resume  ? (uint64_t)resume->valuedouble  : 0;
    out->year             = year    ? year->valueint : 0;
    out->community_rating = rating  ? (float)rating->valuedouble : 0.0f;
    out->played           = played  ? cJSON_IsTrue(played) : 0;

    /* Check ImageTags for Primary */
    cJSON *img_tags = cJSON_GetObjectItem(j, "ImageTags");
    cJSON *primary  = img_tags ? cJSON_GetObjectItem(img_tags, "Primary") : NULL;
    if (primary && id) {
        snprintf(out->thumb_url, sizeof(out->thumb_url),
                 "%s/Items/%s/Images/Primary?maxWidth=128&maxHeight=128&quality=70",
                 base_url, id);
    }
}

int api_get_items(ApiContext *ctx, const char *user_id, const char *token,
                  int start, int limit, ItemList *out) {
    char url[1024], *resp;
    size_t resp_len;

    snprintf(url, sizeof(url),
        "%s/Users/%s/Items"
        "?ParentId=%s"
        "&StartIndex=%d&Limit=%d"
        "&SortBy=SortName&SortOrder=Ascending"
        "&IncludeItemTypes=Movie,Series,Episode,Season"
        "&Fields=Overview,UserData,ImageTags,RunTimeTicks"
        "&ImageTypeLimit=1&Recursive=false",
        ctx->base_url, user_id,
        ctx->current_parent_id,
        start, limit);

    int rc = http_get(ctx, url, token, &resp, &resp_len);
    if (rc != 0) return rc;

    cJSON *json = cJSON_Parse(resp);
    if (!json) return -10;

    cJSON *items = cJSON_GetObjectItem(json, "Items");
    cJSON *total = cJSON_GetObjectItem(json, "TotalRecordCount");
    if (!cJSON_IsArray(items)) { cJSON_Delete(json); return -11; }

    int count = cJSON_GetArraySize(items);
    out->items = (Item *)calloc(count, sizeof(Item));
    out->count = 0;
    out->total_count = total ? total->valueint : count;
    out->start = start;
    out->limit = limit;

    for (int i = 0; i < count; i++) {
        parse_item(cJSON_GetArrayItem(items, i), &out->items[out->count], ctx->base_url);
        out->count++;
    }

    cJSON_Delete(json);
    return 0;
}

int api_get_item_detail(ApiContext *ctx, const char *user_id,
                        const char *item_id, const char *token,
                        ApiItemDetail *out) {
    char url[512], *resp;
    size_t resp_len;

    snprintf(url, sizeof(url),
        "%s/Users/%s/Items/%s"
        "?Fields=Overview,Genres,People,UserData,ImageTags,RunTimeTicks,MediaSources",
        ctx->base_url, user_id, item_id);

    int rc = http_get(ctx, url, token, &resp, &resp_len);
    if (rc != 0) return rc;

    cJSON *json = cJSON_Parse(resp);
    if (!json) return -10;

    parse_item(json, &out->base, ctx->base_url);

    /* Genres */
    out->genre_count = 0;
    cJSON *genres = cJSON_GetObjectItem(json, "Genres");
    if (cJSON_IsArray(genres)) {
        int n = cJSON_GetArraySize(genres);
        for (int i = 0; i < n && i < 3; i++) {
            const char *g = cJSON_GetStringValue(cJSON_GetArrayItem(genres, i));
            if (g) strncpy(out->genres[out->genre_count++], g, 63);
        }
    }

    /* People (cast) */
    out->cast_count = 0;
    cJSON *people = cJSON_GetObjectItem(json, "People");
    if (cJSON_IsArray(people)) {
        int n = cJSON_GetArraySize(people);
        for (int i = 0; i < n && out->cast_count < 5; i++) {
            cJSON *p = cJSON_GetArrayItem(people, i);
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(p, "Type"));
            const char *pname = cJSON_GetStringValue(cJSON_GetObjectItem(p, "Name"));
            if (role && strcmp(role, "Actor") == 0 && pname)
                strncpy(out->cast[out->cast_count++], pname, 63);
        }
    }

    cJSON *ep  = cJSON_GetObjectItem(json, "IndexNumber");
    cJSON *sea = cJSON_GetObjectItem(json, "ParentIndexNumber");
    cJSON *ser = cJSON_GetObjectItem(json, "SeriesName");

    out->episode_number = ep  ? ep->valueint  : 0;
    out->season_number  = sea ? sea->valueint : 0;
    if (ser) {
        const char *sn = cJSON_GetStringValue(ser);
        if (sn) strncpy(out->series_name, sn, sizeof(out->series_name) - 1);
    }

    cJSON_Delete(json);
    return 0;
}

int api_get_resume_items(ApiContext *ctx, const char *user_id,
                         const char *token, ItemList **out) {
    char url[512], *resp;
    size_t resp_len;

    snprintf(url, sizeof(url),
        "%s/Users/%s/Items/Resume"
        "?Limit=10&Fields=UserData,ImageTags,RunTimeTicks"
        "&IncludeItemTypes=Movie,Episode&MediaTypes=Video",
        ctx->base_url, user_id);

    int rc = http_get(ctx, url, token, &resp, &resp_len);
    if (rc != 0) return rc;

    *out = (ItemList *)calloc(1, sizeof(ItemList));
    if (!*out) return -1;

    cJSON *json = cJSON_Parse(resp);
    if (!json) return -10;

    cJSON *items = cJSON_GetObjectItem(json, "Items");
    if (!cJSON_IsArray(items)) { cJSON_Delete(json); return -11; }

    int count = cJSON_GetArraySize(items);
    (*out)->items = (Item *)calloc(count, sizeof(Item));
    (*out)->count = 0;

    for (int i = 0; i < count; i++) {
        parse_item(cJSON_GetArrayItem(items, i), &(*out)->items[(*out)->count], ctx->base_url);
        (*out)->count++;
    }

    cJSON_Delete(json);
    return 0;
}

/* ── Stream URL ──────────────────────────────────────────────────────────── */
/*
 * Build a direct HLS stream URL using Jellyfin's built-in transcoder.
 * Jellyfin transcodes to H.264 Baseline + AAC which the 3DS can play.
 */
void api_build_stream_url(ApiContext *ctx, const char *item_id,
                          const char *token, int width, int height,
                          char *out, size_t out_size) {
    int video_bitrate = (width <= 256) ? 200000 : 500000;
    int audio_bitrate = 64000;

    snprintf(out, out_size,
        "%s/Videos/%s/master.m3u8"
        "?VideoCodec=h264"
        "&AudioCodec=aac"
        "&MaxWidth=%d&MaxHeight=%d"
        "&MaxFramerate=30"
        "&VideoBitrate=%d"
        "&AudioBitrate=%d"
        "&AudioChannels=2"
        "&AudioSampleRate=44100"
        "&h264-profile=baseline"
        "&h264-level=31"
        "&SegmentLength=4"
        "&MinSegments=2"
        "&BreakOnNonKeyFrames=True"
        "&DeviceId=%s"
        "&api_key=%s",
        ctx->base_url, item_id,
        width, height,
        video_bitrate, audio_bitrate,
        DEVICE_ID, token);
}

/* ── Navigation ──────────────────────────────────────────────────────────── */

void api_set_browse_parent(ApiContext *ctx, const char *parent_id) {
    if (ctx->browse_stack_depth < BROWSE_STACK_MAX)
        strncpy(ctx->browse_stack[ctx->browse_stack_depth++],
                ctx->current_parent_id, ITEM_ID_LEN - 1);
    strncpy(ctx->current_parent_id, parent_id, sizeof(ctx->current_parent_id) - 1);
}

int api_can_go_back(ApiContext *ctx) {
    return ctx->browse_stack_depth > 0;
}

void api_go_back(ApiContext *ctx) {
    if (ctx->browse_stack_depth > 0) {
        ctx->browse_stack_depth--;
        strncpy(ctx->current_parent_id,
                ctx->browse_stack[ctx->browse_stack_depth],
                sizeof(ctx->current_parent_id) - 1);
    }
}

/* ── Playback Reporting ──────────────────────────────────────────────────── */
/*
 * POST /Sessions/Playing          — playback started
 * POST /Sessions/Playing/Progress — progress update
 * POST /Sessions/Playing/Stopped  — playback ended
 */
int api_report_playback_start(ApiContext *ctx, const char *item_id,
                              const char *user_id, const char *token) {
    char url[512], body[512], *resp;
    size_t resp_len;

    snprintf(url, sizeof(url), "%s/Sessions/Playing", ctx->base_url);
    snprintf(body, sizeof(body),
        "{\"ItemId\":\"%s\",\"UserId\":\"%s\","
        "\"PlayMethod\":\"Transcode\",\"MediaSourceId\":\"%s\"}",
        item_id, user_id, item_id);

    return http_post(ctx, url, token, body, &resp, &resp_len);
}

int api_report_playback_progress(ApiContext *ctx, const char *item_id,
                                 uint64_t pos_ticks, int is_paused,
                                 const char *user_id, const char *token) {
    char url[512], body[512], *resp;
    size_t resp_len;

    snprintf(url, sizeof(url), "%s/Sessions/Playing/Progress", ctx->base_url);
    snprintf(body, sizeof(body),
        "{\"ItemId\":\"%s\",\"UserId\":\"%s\","
        "\"PositionTicks\":%llu,\"IsPaused\":%s,"
        "\"PlayMethod\":\"Transcode\"}",
        item_id, user_id,
        (unsigned long long)pos_ticks,
        is_paused ? "true" : "false");

    return http_post(ctx, url, token, body, &resp, &resp_len);
}

int api_report_playback_stop(ApiContext *ctx, const char *item_id,
                             uint64_t pos_ticks,
                             const char *user_id, const char *token) {
    char url[512], body[512], *resp;
    size_t resp_len;

    snprintf(url, sizeof(url), "%s/Sessions/Playing/Stopped", ctx->base_url);
    snprintf(body, sizeof(body),
        "{\"ItemId\":\"%s\",\"UserId\":\"%s\","
        "\"PositionTicks\":%llu}",
        item_id, user_id,
        (unsigned long long)pos_ticks);

    return http_post(ctx, url, token, body, &resp, &resp_len);
}

void item_list_free(ItemList *list) {
    if (!list) return;
    free(list->items);
    list->items = NULL;
    list->count = 0;
}
