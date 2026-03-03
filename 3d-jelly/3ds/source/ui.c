/**
 * ui.c - User interface implementation for 3D Jelly
 * Jellyfin-inspired dark theme with citro2d rendering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>
#include "ui.h"
#include "player.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void draw_rounded_rect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, color);
}

static void draw_text(C2D_Font font, const char *text, float x, float y, float size, u32 color) {
    C2D_TextBuf tbuf = C2D_TextBufNew(256);
    C2D_Text t;
    C2D_TextFontParse(&t, font, tbuf, text);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, size, size, color);
    C2D_TextBufDelete(tbuf);
}

static void draw_text_centered(C2D_Font font, const char *text, float y, float w, float size, u32 color) {
    C2D_TextBuf tbuf = C2D_TextBufNew(256);
    C2D_Text t;
    C2D_TextFontParse(&t, font, tbuf, text);
    C2D_TextOptimize(&t);
    
    float tw = 0, th = 0;
    C2D_TextGetDimensions(&t, size, size, &tw, &th);
    float x = (w - tw) / 2.0f;
    
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, size, size, color);
    C2D_TextBufDelete(tbuf);
}

/* Format ticks (100ns) to "HH:MM:SS" string */
static void format_ticks(uint64_t ticks, char *out, int size) {
    uint64_t secs = ticks / 10000000ULL;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    if (h > 0)
        snprintf(out, size, "%d:%02d:%02d", h, m, s);
    else
        snprintf(out, size, "%d:%02d", m, s);
}

/* ── Init/Free ───────────────────────────────────────────────────────────── */

UiContext *ui_init(Config *config) {
    UiContext *ctx = (UiContext *)calloc(1, sizeof(UiContext));
    if (!ctx) return NULL;
    
    ctx->config = config;
    ctx->browse_cols = 4;  /* 4 items per row in grid view */
    
    /* Create render targets */
    ctx->top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    ctx->bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    
    /* Load system font */
    ctx->font = C2D_FontLoadSystem(CFG_REGION_USA);
    
    return ctx;
}

void ui_free(UiContext *ctx) {
    if (!ctx) return;
    if (ctx->font) C2D_FontFree(ctx->font);
    free(ctx);
}

void ui_render_frame(UiContext *ctx) {
    /* Draw toast if active */
    if (ctx->toast.active) {
        u64 now = osGetTime();
        if (now < ctx->toast.show_until) {
            /* Draw on bottom screen */
            C2D_SceneBegin(ctx->bottom);
            float tw = 320.0f;
            draw_rounded_rect(0, 210, tw, 30, C2D_Color32(0x00, 0x64, 0x8A, 0xDD));
            draw_text_centered(ctx->font, ctx->toast.msg, 216, tw, 0.45f, COL_TEXT);
        } else {
            ctx->toast.active = 0;
        }
    }
}

/* ── Splash ──────────────────────────────────────────────────────────────── */

void ui_draw_splash(UiContext *ctx, const char *msg) {
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    /* Logo text */
    draw_text_centered(ctx->font, "3D Jelly", 80, 400, 0.9f, COL_PRIMARY);
    draw_text_centered(ctx->font, "Jellyfin for Nintendo 3DS", 115, 400, 0.4f, COL_TEXT_DIM);
    draw_text_centered(ctx->font, msg, 160, 400, 0.4f, COL_ACCENT);
    
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    draw_text_centered(ctx->font, "Loading...", 120, 320, 0.45f, COL_TEXT_DIM);
}

/* ── Setup Wizard ────────────────────────────────────────────────────────── */

SetupResult ui_draw_setup(UiContext *ctx, Config *config) {
    SetupResult result = {0};
    
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    draw_text_centered(ctx->font, "3D Jelly Setup", 30, 400, 0.8f, COL_PRIMARY);
    draw_text_centered(ctx->font, "Connect to your Jellyfin server", 70, 400, 0.4f, COL_TEXT_DIM);
    
    draw_text(ctx->font, "Proxy Server IP:", 20, 110, 0.45f, COL_TEXT);
    draw_text(ctx->font, config->jellyfin_host, 20, 130, 0.5f, COL_ACCENT);
    
    draw_text(ctx->font, "Port:", 20, 155, 0.45f, COL_TEXT);
    char port_str[16]; snprintf(port_str, sizeof(port_str), "%d", config->jellyfin_port);
    draw_text(ctx->font, port_str, 70, 155, 0.5f, COL_ACCENT);
    
    draw_text_centered(ctx->font, "Press A to set IP, B to set port, START to continue",
                       200, 400, 0.38f, COL_TEXT_DIM);
    
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    
    draw_rounded_rect(10, 10, 300, 40, COL_SURFACE);
    draw_text(ctx->font, "A: Enter proxy IP", 20, 20, 0.45f, COL_TEXT);
    draw_text(ctx->font, "B: Enter proxy port", 20, 38, 0.45f, COL_TEXT);
    draw_text(ctx->font, "START: Done", 20, 56, 0.45f, COL_ACCENT);
    
    u32 keys = hidKeysDown();
    
    if (keys & KEY_A) {
        ui_get_text_input("Jellyfin Server IP", config->jellyfin_host, sizeof(config->jellyfin_host));
    }
    if (keys & KEY_B) {
        char port_buf[8] = {0};
        snprintf(port_buf, sizeof(port_buf), "%d", config->jellyfin_port);
        ui_get_text_input("Port (default 8096)", port_buf, sizeof(port_buf));
        config->jellyfin_port = atoi(port_buf);
    }
    if (keys & KEY_START) {
        result.done = 1;
    }
    
    return result;
}

/* ── Auth Screen ─────────────────────────────────────────────────────────── */

static char s_auth_user[64] = "";
static char s_auth_pass[64] = "";
static int  s_auth_field = 0;  /* 0=username, 1=password */

AuthResult ui_draw_auth(UiContext *ctx) {
    AuthResult result = {0};
    
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    draw_text_centered(ctx->font, "Sign In to Jellyfin", 50, 400, 0.7f, COL_PRIMARY);
    
    /* Username field */
    u32 user_col = (s_auth_field == 0) ? COL_SELECTED : COL_SURFACE;
    draw_rounded_rect(60, 100, 280, 35, user_col);
    draw_text(ctx->font, "Username", 70, 107, 0.38f, COL_TEXT_DIM);
    draw_text(ctx->font, s_auth_user[0] ? s_auth_user : "tap to enter...", 70, 120, 0.45f,
              s_auth_user[0] ? COL_TEXT : COL_TEXT_DIM);
    
    /* Password field */
    u32 pass_col = (s_auth_field == 1) ? COL_SELECTED : COL_SURFACE;
    draw_rounded_rect(60, 150, 280, 35, pass_col);
    draw_text(ctx->font, "Password", 70, 157, 0.38f, COL_TEXT_DIM);
    /* Show dots for password */
    char pass_dots[64] = "";
    for (int i = 0; s_auth_pass[i]; i++) strcat(pass_dots, "•");
    draw_text(ctx->font, pass_dots[0] ? pass_dots : "tap to enter...", 70, 170, 0.45f,
              pass_dots[0] ? COL_TEXT : COL_TEXT_DIM);
    
    /* Sign in button */
    draw_rounded_rect(100, 205, 200, 30, COL_PRIMARY);
    draw_text_centered(ctx->font, "Sign In  (START)", 212, 400, 0.45f, COL_BG);
    
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    draw_text(ctx->font, "A: Enter username   B: Enter password", 10, 100, 0.4f, COL_TEXT_DIM);
    draw_text(ctx->font, "START: Sign In", 10, 120, 0.4f, COL_ACCENT);
    
    u32 keys = hidKeysDown();
    
    if (keys & KEY_A) {
        ui_get_text_input("Jellyfin Username", s_auth_user, sizeof(s_auth_user));
        s_auth_field = 0;
    }
    if (keys & KEY_B) {
        ui_get_text_input("Jellyfin Password", s_auth_pass, sizeof(s_auth_pass));
        s_auth_field = 1;
    }
    if (keys & KEY_START) {
        result.done = 1;
        strncpy(result.username, s_auth_user, sizeof(result.username) - 1);
        strncpy(result.password, s_auth_pass, sizeof(result.password) - 1);
    }
    
    return result;
}

/* ── Home Screen ─────────────────────────────────────────────────────────── */

void ui_set_home_items(UiContext *ctx, ItemList *items) {
    ctx->home_items = items;
    ctx->home_sel = 0;
    ctx->home_loaded = 1;
}

void ui_draw_home(UiContext *ctx, const char *username) {
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    /* Header */
    draw_rounded_rect(0, 0, 400, 40, COL_SURFACE);
    draw_text(ctx->font, "3D Jelly", 10, 10, 0.55f, COL_PRIMARY);
    char welcome[80];
    snprintf(welcome, sizeof(welcome), "Welcome, %s", username);
    draw_text(ctx->font, welcome, 200, 12, 0.42f, COL_TEXT_DIM);
    
    /* Continue Watching */
    draw_text(ctx->font, "Continue Watching", 10, 50, 0.5f, COL_TEXT);
    
    if (!ctx->home_items || ctx->home_items->count == 0) {
        draw_text_centered(ctx->font, "Nothing in progress", 100, 400, 0.45f, COL_TEXT_DIM);
        draw_text_centered(ctx->font, "Press L to browse your libraries", 120, 400, 0.4f, COL_TEXT_DIM);
    } else {
        /* Draw resume item cards */
        for (int i = 0; i < ctx->home_items->count && i < 3; i++) {
            Item *item = &ctx->home_items->items[i];
            float x = 10 + i * 130;
            float y = 70;
            
            u32 border = (i == ctx->home_sel) ? COL_PRIMARY : COL_SURFACE;
            draw_rounded_rect(x - 2, y - 2, 124, 84, border);
            draw_rounded_rect(x, y, 120, 80, COL_SURFACE);
            
            /* Resume progress bar */
            if (item->runtime_ticks > 0 && item->resume_ticks > 0) {
                float progress = (float)item->resume_ticks / item->runtime_ticks;
                if (progress > 1.0f) progress = 1.0f;
                draw_rounded_rect(x, y + 74, 120 * progress, 4, COL_RESUME);
                draw_rounded_rect(120 * progress + x, y + 74, 120 * (1.0f - progress), 4,
                                  COL_SURFACE);
            }
            
            /* Item name (truncated) */
            char name_trunc[20];
            strncpy(name_trunc, item->name, 18);
            if (strlen(item->name) > 18) { name_trunc[17] = '.'; name_trunc[18] = '.'; name_trunc[19] = '\0'; }
            draw_text(ctx->font, name_trunc, x + 2, y + 56, 0.38f, COL_TEXT);
        }
    }
    
    /* Controls hint */
    draw_text(ctx->font, "L: Libraries  START: Settings  SELECT: Resume", 10, 218, 0.36f, COL_TEXT_DIM);
    
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    draw_rounded_rect(0, 0, 320, 240, COL_BG);
    draw_text_centered(ctx->font, "3D Jelly", 80, 320, 0.8f, COL_PRIMARY);
    draw_text_centered(ctx->font, "Jellyfin for 3DS CFW", 115, 320, 0.42f, COL_TEXT_DIM);
    draw_text_centered(ctx->font, "Max resolution: 240p", 140, 320, 0.4f, COL_ACCENT);
}

Item *ui_get_selected_home_item(UiContext *ctx) {
    if (!ctx->home_items || ctx->home_items->count == 0) return NULL;
    if (ctx->home_sel < 0 || ctx->home_sel >= ctx->home_items->count) return NULL;
    return &ctx->home_items->items[ctx->home_sel];
}

/* ── Libraries ───────────────────────────────────────────────────────────── */

void ui_set_libraries(UiContext *ctx, ApiLibraryList *libs) {
    ctx->libraries = *libs;
    ctx->library_sel = 0;
    ctx->libraries_loaded = 1;
}

void ui_draw_libraries(UiContext *ctx) {
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    draw_rounded_rect(0, 0, 400, 35, COL_SURFACE);
    draw_text(ctx->font, "Libraries", 10, 8, 0.6f, COL_PRIMARY);
    
    for (int i = 0; i < ctx->libraries.count; i++) {
        Library *lib = &ctx->libraries.items[i];
        float y = 45 + i * 38;
        
        u32 bg = (i == ctx->library_sel) ? COL_SELECTED : COL_SURFACE;
        draw_rounded_rect(10, y, 380, 32, bg);
        draw_text(ctx->font, lib->name, 18, y + 8, 0.52f, COL_TEXT);
        draw_text(ctx->font, lib->type, 340, y + 10, 0.38f, COL_TEXT_DIM);
    }
    
    draw_text(ctx->font, "A:Open  B:Back  D-Pad:Navigate", 10, 225, 0.36f, COL_TEXT_DIM);
    
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    draw_text_centered(ctx->font, "Select a library", 110, 320, 0.45f, COL_TEXT_DIM);
    
    u32 keys = hidKeysDown();
    if (keys & KEY_UP   && ctx->library_sel > 0) ctx->library_sel--;
    if (keys & KEY_DOWN && ctx->library_sel < ctx->libraries.count - 1) ctx->library_sel++;
}

Library *ui_get_selected_library(UiContext *ctx) {
    if (ctx->libraries.count == 0) return NULL;
    return &ctx->libraries.items[ctx->library_sel];
}

/* ── Browse Grid ─────────────────────────────────────────────────────────── */

void ui_set_browse_items(UiContext *ctx, ItemList *items) {
    ctx->browse_items = items;
    ctx->browse_sel = 0;
    ctx->browse_loaded = 1;
}

void ui_draw_browse(UiContext *ctx) {
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    if (!ctx->browse_items || ctx->browse_items->count == 0) {
        draw_text_centered(ctx->font, "No items found", 110, 400, 0.5f, COL_TEXT_DIM);
        return;
    }
    
    /* Grid view: 4 columns, cell 96x90 */
    int cols = ctx->browse_cols;
    float cell_w = 96;
    float cell_h = 90;
    float pad = 4;
    
    for (int i = 0; i < ctx->browse_items->count && i < 8; i++) {
        Item *item = &ctx->browse_items->items[i];
        int col = i % cols;
        int row = i / cols;
        float x = pad + col * (cell_w + pad);
        float y = pad + row * (cell_h + pad);
        
        u32 bg = (i == ctx->browse_sel) ? COL_SELECTED : COL_SURFACE;
        if (i == ctx->browse_sel)
            draw_rounded_rect(x - 2, y - 2, cell_w + 4, cell_h + 4, COL_PRIMARY);
        draw_rounded_rect(x, y, cell_w, cell_h, bg);
        
        /* Played checkmark */
        if (item->played) {
            draw_rounded_rect(x + cell_w - 14, y + 2, 12, 12, COL_PLAYED);
            draw_text(ctx->font, "✓", x + cell_w - 13, y + 2, 0.4f, COL_BG);
        }
        
        /* Item name */
        char name[16];
        strncpy(name, item->name, 14);
        name[14] = '\0';
        draw_text(ctx->font, name, x + 2, y + cell_h - 20, 0.36f, COL_TEXT);
        
        /* Year */
        if (item->year > 0) {
            char year_str[8];
            snprintf(year_str, sizeof(year_str), "%d", item->year);
            draw_text(ctx->font, year_str, x + 2, y + cell_h - 8, 0.33f, COL_TEXT_DIM);
        }
    }
    
    /* Page info */
    char page_info[64];
    snprintf(page_info, sizeof(page_info), "%d-%d of %d",
             ctx->browse_items->start + 1,
             ctx->browse_items->start + ctx->browse_items->count,
             ctx->browse_items->total_count);
    draw_text(ctx->font, page_info, 10, 224, 0.35f, COL_TEXT_DIM);
    draw_text(ctx->font, "L/R: Pages  A:Select  B:Back  X:Res", 180, 224, 0.35f, COL_TEXT_DIM);
    
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    
    /* Show selected item info on bottom screen */
    if (ctx->browse_sel < ctx->browse_items->count) {
        Item *sel = &ctx->browse_items->items[ctx->browse_sel];
        draw_text(ctx->font, sel->name, 10, 15, 0.55f, COL_TEXT);
        if (sel->year > 0) {
            char yr[8]; snprintf(yr, sizeof(yr), "%d", sel->year);
            draw_text(ctx->font, yr, 10, 40, 0.42f, COL_TEXT_DIM);
        }
        if (sel->community_rating > 0) {
            char rat[16]; snprintf(rat, sizeof(rat), "★ %.1f", sel->community_rating);
            draw_text(ctx->font, rat, 60, 40, 0.42f, COL_RESUME);
        }
        if (sel->overview[0]) {
            /* Word-wrapped overview — simplified, just truncate */
            char ov[80];
            strncpy(ov, sel->overview, 78); ov[78] = '\0';
            draw_text(ctx->font, ov, 10, 65, 0.37f, COL_TEXT_DIM);
        }
    }
}

void ui_navigate(UiContext *ctx, NavDir dir) {
    if (!ctx->browse_items) return;
    int count = ctx->browse_items->count;
    int cols = ctx->browse_cols;
    
    switch (dir) {
        case DIR_UP:    ctx->browse_sel -= cols; break;
        case DIR_DOWN:  ctx->browse_sel += cols; break;
        case DIR_LEFT:  ctx->browse_sel -= 1;    break;
        case DIR_RIGHT: ctx->browse_sel += 1;    break;
    }
    
    if (ctx->browse_sel < 0) ctx->browse_sel = 0;
    if (ctx->browse_sel >= count) ctx->browse_sel = count - 1;
}

Item *ui_get_selected_browse_item(UiContext *ctx) {
    if (!ctx->browse_items || ctx->browse_items->count == 0) return NULL;
    if (ctx->browse_sel < 0 || ctx->browse_sel >= ctx->browse_items->count) return NULL;
    return &ctx->browse_items->items[ctx->browse_sel];
}

/* ── Item Detail ─────────────────────────────────────────────────────────── */

void ui_set_item_detail(UiContext *ctx, ApiItemDetail *detail) {
    ctx->detail = *detail;
    ctx->detail_loaded = 1;
}

void ui_draw_item_detail(UiContext *ctx) {
    ApiItemDetail *d = &ctx->detail;
    
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    /* Title */
    draw_text(ctx->font, d->base.name, 10, 10, 0.6f, COL_TEXT);
    
    /* Series info */
    if (d->series_name[0]) {
        char ep_info[64];
        snprintf(ep_info, sizeof(ep_info), "%s  S%02d E%02d",
                 d->series_name, d->season_number, d->episode_number);
        draw_text(ctx->font, ep_info, 10, 38, 0.4f, COL_TEXT_DIM);
    }
    
    /* Year + rating */
    char meta[64] = "";
    if (d->base.year > 0) {
        char y[8]; snprintf(y, sizeof(y), "%d", d->base.year);
        strncat(meta, y, sizeof(meta) - strlen(meta) - 1);
    }
    if (d->base.community_rating > 0) {
        char r[16]; snprintf(r, sizeof(r), "  ★%.1f", d->base.community_rating);
        strncat(meta, r, sizeof(meta) - strlen(meta) - 1);
    }
    draw_text(ctx->font, meta, 10, 55, 0.42f, COL_RESUME);
    
    /* Genres */
    char genres[64] = "";
    for (int i = 0; i < d->genre_count; i++) {
        if (i > 0) strncat(genres, " · ", sizeof(genres) - strlen(genres) - 1);
        strncat(genres, d->genres[i], sizeof(genres) - strlen(genres) - 1);
    }
    if (genres[0]) draw_text(ctx->font, genres, 10, 73, 0.38f, COL_TEXT_DIM);
    
    /* Overview */
    if (d->base.overview[0]) {
        draw_text(ctx->font, d->base.overview, 10, 95, 0.38f, COL_TEXT_DIM);
    }
    
    /* Resolution mode indicator */
    const char *res = (ctx->config->resolution_mode == RES_144P) ? "144p" : "240p";
    char res_label[32]; snprintf(res_label, sizeof(res_label), "Stream: %s  (X to toggle)", res);
    draw_text(ctx->font, res_label, 10, 210, 0.37f, COL_ACCENT);
    
    draw_text(ctx->font, "A:Play  B:Back  X:Toggle Res", 10, 225, 0.36f, COL_TEXT_DIM);
    
    /* Bottom screen: play button + cast */
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    
    draw_rounded_rect(60, 10, 200, 45, COL_PRIMARY);
    draw_text_centered(ctx->font, "▶  PLAY  (A)", 28, 320, 0.55f, COL_BG);
    
    /* Resume time */
    if (d->base.resume_ticks > 0 && d->base.runtime_ticks > 0) {
        char resume_str[32];
        snprintf(resume_str, sizeof(resume_str), "Resume from: ");
        format_ticks(d->base.resume_ticks, resume_str + strlen(resume_str),
                     sizeof(resume_str) - strlen(resume_str));
        draw_text_centered(ctx->font, resume_str, 62, 320, 0.4f, COL_RESUME);
    }
    
    /* Cast */
    if (d->cast_count > 0) {
        draw_text(ctx->font, "Cast:", 10, 95, 0.42f, COL_TEXT_DIM);
        char cast_str[128] = "";
        for (int i = 0; i < d->cast_count; i++) {
            if (i > 0) strncat(cast_str, ", ", sizeof(cast_str) - strlen(cast_str) - 1);
            strncat(cast_str, d->cast[i], sizeof(cast_str) - strlen(cast_str) - 1);
        }
        draw_text(ctx->font, cast_str, 10, 112, 0.38f, COL_TEXT_DIM);
    }
}

/* ── Player Overlay ──────────────────────────────────────────────────────── */

void ui_draw_player_overlay(UiContext *ctx, PlayerContext *player) {
    /* Bottom screen: playback controls */
    C2D_TargetClear(ctx->bottom, C2D_Color32(0, 0, 0, 200));
    C2D_SceneBegin(ctx->bottom);
    
    /* Progress bar */
    float progress = 0.0f;
    if (player->duration_ticks > 0)
        progress = (float)((double)player->position_ticks / player->duration_ticks);
    if (progress > 1.0f) progress = 1.0f;
    
    draw_rounded_rect(10, 20, 300, 6, COL_SURFACE);
    if (progress > 0)
        draw_rounded_rect(10, 20, 300.0f * progress, 6, COL_PRIMARY);
    
    /* Time */
    char pos_str[16], dur_str[16];
    format_ticks(player->position_ticks, pos_str, sizeof(pos_str));
    format_ticks(player->duration_ticks, dur_str, sizeof(dur_str));
    
    draw_text(ctx->font, pos_str, 10, 32, 0.4f, COL_TEXT);
    draw_text(ctx->font, dur_str, 270, 32, 0.4f, COL_TEXT_DIM);
    
    /* Pause indicator */
    if (player->is_paused) {
        draw_text_centered(ctx->font, "⏸ PAUSED", 55, 320, 0.55f, COL_ACCENT);
    }
    
    /* Controls */
    draw_text_centered(ctx->font, "A:Pause  ←→:±10s  ↑↓:±5min  B:Stop", 200, 320, 0.35f, COL_TEXT_DIM);
    
    /* Resolution badge */
    const char *res = (ctx->config->resolution_mode == RES_144P) ? "144p" : "240p";
    draw_rounded_rect(270, 190, 40, 16, COL_SURFACE);
    draw_text(ctx->font, res, 274, 193, 0.38f, COL_ACCENT);
}

/* ── Settings ────────────────────────────────────────────────────────────── */

void ui_draw_settings(UiContext *ctx, Config *config) {
    const char *res_options[] = {"240p (400x240)", "144p (256x144)"};
    
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    draw_rounded_rect(0, 0, 400, 35, COL_SURFACE);
    draw_text(ctx->font, "Settings", 10, 8, 0.6f, COL_PRIMARY);
    
    const char *settings_labels[] = {
        "Resolution", "Server IP", "Server Port", "Show Thumbnails", "Logout"
    };
    const int settings_count = 5;
    
    for (int i = 0; i < settings_count; i++) {
        float y = 45 + i * 36;
        u32 bg = (i == ctx->settings_sel) ? COL_SELECTED : COL_SURFACE;
        draw_rounded_rect(10, y, 380, 30, bg);
        draw_text(ctx->font, settings_labels[i], 18, y + 7, 0.48f, COL_TEXT);
        
        /* Current value */
        char val[64] = "";
        if (i == 0) strncpy(val, res_options[config->resolution_mode], sizeof(val) - 1);
        else if (i == 1) strncpy(val, config->jellyfin_host, sizeof(val) - 1);
        else if (i == 2) snprintf(val, sizeof(val), "%d", config->jellyfin_port);
        else if (i == 3) strncpy(val, config->show_thumbnails ? "On" : "Off", sizeof(val) - 1);
        
        draw_text(ctx->font, val, 240, y + 9, 0.4f, COL_TEXT_DIM);
    }
    
    draw_text(ctx->font, "A:Edit  B:Back  D-Pad:Navigate", 10, 225, 0.36f, COL_TEXT_DIM);
    
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    draw_text_centered(ctx->font, "3D Jelly v1.0.0", 100, 320, 0.45f, COL_TEXT_DIM);
    draw_text_centered(ctx->font, "Max resolution: 240p", 120, 320, 0.4f, COL_ACCENT);
    
    u32 keys = hidKeysDown();
    if (keys & KEY_UP   && ctx->settings_sel > 0) ctx->settings_sel--;
    if (keys & KEY_DOWN && ctx->settings_sel < 4) ctx->settings_sel++;
}

void settings_apply_selection(UiContext *ctx, Config *config) {
    switch (ctx->settings_sel) {
        case 0: /* Resolution toggle */
            config->resolution_mode = (config->resolution_mode == RES_240P) ? RES_144P : RES_240P;
            break;
        case 1: /* Server IP */
            ui_get_text_input("Jellyfin Server IP", config->jellyfin_host, sizeof(config->jellyfin_host));
            break;
        case 2: { /* Server Port */
            char port_buf[8];
            snprintf(port_buf, sizeof(port_buf), "%d", config->jellyfin_port);
            ui_get_text_input("Port (default 8096)", port_buf, sizeof(port_buf));
            config->jellyfin_port = atoi(port_buf);
            break;
        }
        case 3: /* Thumbnails toggle */
            config->show_thumbnails = !config->show_thumbnails;
            break;
        case 4: /* Logout */
            memset(config->token, 0, sizeof(config->token));
            memset(config->user_id, 0, sizeof(config->user_id));
            memset(config->username, 0, sizeof(config->username));
            break;
    }
}

/* ── Error ───────────────────────────────────────────────────────────────── */

void ui_draw_error(UiContext *ctx, const char *msg) {
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    
    draw_rounded_rect(50, 80, 300, 80, C2D_Color32(0x7B, 0x00, 0x00, 0xFF));
    draw_text_centered(ctx->font, "Error", 95, 400, 0.6f, C2D_Color32(0xFF, 0x50, 0x50, 0xFF));
    draw_text_centered(ctx->font, msg, 125, 400, 0.4f, COL_TEXT);
    draw_text_centered(ctx->font, "Press B to go back", 175, 400, 0.38f, COL_TEXT_DIM);
}

/* ── Toast ───────────────────────────────────────────────────────────────── */

void ui_show_toast(UiContext *ctx, const char *msg, int duration_ms) {
    strncpy(ctx->toast.msg, msg, sizeof(ctx->toast.msg) - 1);
    ctx->toast.show_until = osGetTime() + (u64)duration_ms;
    ctx->toast.active = 1;
}

/* ── Keyboard Input ──────────────────────────────────────────────────────── */

int ui_get_text_input(const char *hint, char *out, int out_size) {
    SwkbdState kbd;
    swkbdInit(&kbd, SWKBD_TYPE_NORMAL, 2, out_size - 1);
    swkbdSetHintText(&kbd, hint);
    swkbdSetInitialText(&kbd, out);
    swkbdSetFeatures(&kbd, SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET | SWKBD_ALLOW_POWER);
    
    char buf[out_size];
    SwkbdButton btn = swkbdInputText(&kbd, buf, sizeof(buf));
    
    if (btn == SWKBD_BUTTON_CONFIRM) {
        strncpy(out, buf, out_size - 1);
        return 1;
    }
    return 0;
}

int ui_draw_discovery(UiContext *ctx, Config *config) {
    C2D_TargetClear(ctx->top, COL_BG);
    C2D_SceneBegin(ctx->top);
    draw_rounded_rect(0, 0, 400, 38, COL_SURFACE);
    draw_text(ctx->font, "3D Jelly", 10, 8, 0.55f, COL_PRIMARY);
    draw_text(ctx->font, "Select Jellyfin Server", 130, 10, 0.45f, COL_TEXT_DIM);
    ServerDiscoveryResult *disc = &ctx->discovery;
    int total_entries = disc->count + 1;
    if (!ctx->discovery_done) {
        draw_text_centered(ctx->font, "Scanning network...", 110, 400, 0.5f, COL_ACCENT);
        draw_text_centered(ctx->font, "Broadcasting on UDP port 7359", 135, 400, 0.38f, COL_TEXT_DIM);
    } else if (disc->count == 0) {
        draw_text_centered(ctx->font, "No servers found.", 80, 400, 0.45f, COL_TEXT_DIM);
        draw_text_centered(ctx->font, "Select Manual entry or press X to rescan.", 100, 400, 0.4f, COL_TEXT_DIM);
    } else {
        for (int i = 0; i < disc->count; i++) {
            float y = 46 + i * 36;
            u32 bg = (i == ctx->discovery_sel) ? COL_SELECTED : COL_SURFACE;
            draw_rounded_rect(10, y, 380, 30, bg);
            draw_text(ctx->font, disc->servers[i].name,    18, y + 4,  0.5f,  COL_TEXT);
            draw_text(ctx->font, disc->servers[i].address, 18, y + 18, 0.37f, COL_TEXT_DIM);
        }
    }
    if (ctx->discovery_done) {
        int mi = disc->count;
        float y = 46 + mi * 36;
        if (y < 215) {
            u32 bg = (ctx->discovery_sel == mi) ? COL_SELECTED : COL_SURFACE;
            draw_rounded_rect(10, y, 380, 30, bg);
            draw_text(ctx->font, "Manual entry...", 18, y + 8, 0.48f, COL_ACCENT);
        }
    }
    draw_text(ctx->font, "A:Select  UP/DOWN:Navigate  X:Rescan", 10, 226, 0.35f, COL_TEXT_DIM);
    C2D_TargetClear(ctx->bottom, COL_BG);
    C2D_SceneBegin(ctx->bottom);
    draw_text_centered(ctx->font, "3D Jelly", 30, 320, 0.7f, COL_PRIMARY);
    if (ctx->discovery_done && ctx->discovery_sel < disc->count) {
        draw_text_centered(ctx->font, disc->servers[ctx->discovery_sel].name,    80,  320, 0.52f, COL_TEXT);
        draw_text_centered(ctx->font, disc->servers[ctx->discovery_sel].address, 102, 320, 0.4f,  COL_TEXT_DIM);
        draw_text_centered(ctx->font, "Press A to connect", 135, 320, 0.42f, COL_ACCENT);
    } else if (ctx->discovery_done) {
        draw_text_centered(ctx->font, "Enter server address manually", 90,  320, 0.42f, COL_TEXT_DIM);
        draw_text_centered(ctx->font, "e.g. 192.168.1.100  port 8096", 110, 320, 0.4f,  COL_TEXT_DIM);
    }
    u32 keys = hidKeysDown();
    if (keys & KEY_UP   && ctx->discovery_sel > 0)                  ctx->discovery_sel--;
    if (keys & KEY_DOWN && ctx->discovery_sel < total_entries - 1)  ctx->discovery_sel++;
    if (keys & KEY_X) {
        ctx->discovery_done = 0;
        ctx->discovery_sel  = 0;
        memset(&ctx->discovery, 0, sizeof(ctx->discovery));
        api_discover_servers(&ctx->discovery);
        ctx->discovery_done = 1;
    }
    if (keys & KEY_A) {
        if (ctx->discovery_sel < disc->count) {
            DiscoveredServer *s = &disc->servers[ctx->discovery_sel];
            char addr_copy[256];
            strncpy(addr_copy, s->address, sizeof(addr_copy) - 1);
            char *host_start = addr_copy;
            if (strncmp(host_start, "http://",  7) == 0) host_start += 7;
            if (strncmp(host_start, "https://", 8) == 0) host_start += 8;
            char *colon = strrchr(host_start, ':');
            int port = 8096;
            if (colon) { *colon = '\0'; port = atoi(colon + 1); }
            strncpy(config->jellyfin_host, host_start, sizeof(config->jellyfin_host) - 1);
            config->jellyfin_port = port;
            strncpy(config->server_name, s->name, sizeof(config->server_name) - 1);
            return 1;
        } else {
            char host_buf[256]; char port_buf[8];
            strncpy(host_buf, config->jellyfin_host, sizeof(host_buf) - 1);
            snprintf(port_buf, sizeof(port_buf), "%d", config->jellyfin_port);
            ui_get_text_input("Jellyfin Server IP / Hostname", host_buf, sizeof(host_buf));
            ui_get_text_input("Port (default 8096)", port_buf, sizeof(port_buf));
            strncpy(config->jellyfin_host, host_buf, sizeof(config->jellyfin_host) - 1);
            config->jellyfin_port = atoi(port_buf);
            if (config->jellyfin_port <= 0) config->jellyfin_port = 8096;
            strncpy(config->server_name, config->jellyfin_host, sizeof(config->server_name) - 1);
            return 1;
        }
    }
    return 0;
}
