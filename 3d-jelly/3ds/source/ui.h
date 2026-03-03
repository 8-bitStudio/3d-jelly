/**
 * ui.h - User interface for 3D Jelly
 *
 * Screen layout:
 *   Top screen (400x240): Video / art / main content
 *   Bottom screen (320x240): Navigation / controls
 */

#pragma once
#include <3ds.h>
#include <citro2d.h>
#include "api.h"
#include "config.h"
#include "player.h"

/* ── Result types ────────────────────────────────────────────────────────── */
typedef struct { int done; char username[64]; char password[64]; } AuthResult;
typedef struct { int done; } SetupResult;
typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } NavDir;

/* ── Colors (Jellyfin-inspired dark theme) ───────────────────────────────── */
#define COL_BG        C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF)
#define COL_SURFACE   C2D_Color32(0x16, 0x21, 0x3e, 0xFF)
#define COL_PRIMARY   C2D_Color32(0x00, 0xb4, 0xd8, 0xFF)
#define COL_ACCENT    C2D_Color32(0x90, 0xe0, 0xef, 0xFF)
#define COL_TEXT      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_TEXT_DIM  C2D_Color32(0xAA, 0xAA, 0xAA, 0xFF)
#define COL_SELECTED  C2D_Color32(0x00, 0x96, 0xc7, 0xFF)
#define COL_PLAYED    C2D_Color32(0x52, 0xb7, 0x88, 0xFF)
#define COL_RESUME    C2D_Color32(0xff, 0xb7, 0x03, 0xFF)

/* ── Toast ───────────────────────────────────────────────────────────────── */
typedef struct {
    char msg[128];
    u64  show_until;
    int  active;
} Toast;

/* ── UI Context ──────────────────────────────────────────────────────────── */
typedef struct {
    C2D_Font          font;
    C3D_RenderTarget *top;
    C3D_RenderTarget *bottom;
    Toast             toast;
    Config           *config;

    int home_loaded;
    int libraries_loaded;
    int browse_loaded;
    int detail_loaded;

    ItemList       *home_items;
    int             home_sel;

    ApiLibraryList  libraries;
    int             library_sel;

    ItemList       *browse_items;
    int             browse_sel;
    int             browse_cols;

    ApiItemDetail   detail;

    int             settings_sel;

    /* Server discovery */
    ServerDiscoveryResult discovery;
    int                   discovery_sel;
    int                   discovery_done;   /* 1 = scan finished */
} UiContext;

/* ── Functions ───────────────────────────────────────────────────────────── */
UiContext *ui_init(Config *config);
void       ui_free(UiContext *ctx);
void       ui_render_frame(UiContext *ctx);

void ui_draw_splash(UiContext *ctx, const char *msg);

SetupResult ui_draw_setup(UiContext *ctx, Config *config);
AuthResult  ui_draw_auth(UiContext *ctx);

void  ui_draw_home(UiContext *ctx, const char *username);
void  ui_set_home_items(UiContext *ctx, ItemList *items);
Item *ui_get_selected_home_item(UiContext *ctx);

void     ui_draw_libraries(UiContext *ctx);
void     ui_set_libraries(UiContext *ctx, ApiLibraryList *libs);
Library *ui_get_selected_library(UiContext *ctx);

void  ui_draw_browse(UiContext *ctx);
void  ui_set_browse_items(UiContext *ctx, ItemList *items);
Item *ui_get_selected_browse_item(UiContext *ctx);
void  ui_navigate(UiContext *ctx, NavDir dir);

void ui_draw_item_detail(UiContext *ctx);
void ui_set_item_detail(UiContext *ctx, ApiItemDetail *detail);

void ui_draw_player_overlay(UiContext *ctx, PlayerContext *player);

void ui_draw_settings(UiContext *ctx, Config *config);
void settings_apply_selection(UiContext *ctx, Config *config);

void ui_draw_error(UiContext *ctx, const char *msg);

void ui_show_toast(UiContext *ctx, const char *msg, int duration_ms);

int ui_get_text_input(const char *hint, char *out, int out_size);

/**
 * Server discovery screen.
 * Shows a list of auto-discovered Jellyfin servers + manual entry option.
 * Returns 1 and fills config when the user selects a server.
 * Returns 0 while still choosing.
 */
int ui_draw_discovery(UiContext *ctx, Config *config);