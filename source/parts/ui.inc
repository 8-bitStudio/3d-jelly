static void clipped_name(const char *src, char *out, size_t outsz, size_t max_chars)
{
    if (!outsz) {
        return;
    }
    out[0] = 0;
    if (!src) {
        return;
    }

    size_t w = 0;
    size_t chars = 0;
    size_t limit = outsz > 4 ? outsz - 4 : outsz - 1;
    const char *p = src;
    while (*p && chars < max_chars) {
        size_t len = utf8_sequence_len(p);
        if (!len || w + len > limit) {
            break;
        }
        memcpy(out + w, p, len);
        w += len;
        p += len;
        chars++;
    }
    out[w] = 0;
    if (*p && outsz > 4) {
        strcat(out, "...");
    }
}

static void draw_text(float x, float y, float scale, u32 color, const char *fmt, ...)
{
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    C2D_Text t;
    C2D_TextParse(&t, g_text, buf);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static void draw_text_wrap(float x, float y, float scale, float wrap, u32 color, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    C2D_Text t;
    C2D_TextParse(&t, g_text, buf);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap, x, y, 0.5f, scale, scale, color, wrap);
}

static void draw_text_centered(float center_x, float y, float scale, u32 color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    C2D_Text t;
    C2D_TextParse(&t, g_text, buf);
    C2D_TextOptimize(&t);
    float width = 0.0f;
    float height = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &width, &height);
    (void)height;
    C2D_DrawText(&t, C2D_WithColor, center_x - width * 0.5f, y, 0.5f, scale, scale, color);
}

static void draw_text_plain_z(float x, float y, float z, float scale, u32 color, const char *text)
{
    C2D_Text t;
    C2D_TextParse(&t, g_text, text ? text : "");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, z, scale, scale, color);
}

static void draw_text_centered_plain_z(float center_x, float y, float z, float scale, u32 color, const char *text)
{
    C2D_Text t;
    C2D_TextParse(&t, g_text, text ? text : "");
    C2D_TextOptimize(&t);
    float width = 0.0f;
    float height = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &width, &height);
    (void)height;
    C2D_DrawText(&t, C2D_WithColor, center_x - width * 0.5f, y, z, scale, scale, color);
}

static void draw_header(const char *title)
{
    C2D_DrawRectSolid(0, 0, 0, 400, 34, COL_PAPER);
    C2D_DrawRectSolid(0, 33, 0, 400, 2, COL_PRIMARY);
    draw_text(12, 8, 0.55f, COL_WHITE, "%s", tr(TR_APP_TITLE));

    char centered[72];
    clipped_name(title ? title : "", centered, sizeof(centered), 24);
    draw_text_centered(200.0f, 9.0f, 0.48f, COL_MUTED, "%s", centered);
}

static void draw_bottom_help(const char *line1, const char *line2)
{
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COL_PAPER);
    C2D_DrawRectSolid(0, 0, 0, 320, 4, COL_PRIMARY);
    draw_text_wrap(12, 15, 0.48f, 296, COL_WHITE, "%s", line1);
    draw_text_wrap(12, 55, 0.43f, 296, COL_MUTED, "%s", line2);
    draw_text_wrap(12, 168, 0.40f, 296, COL_MUTED, "%s", g_status);
}

static bool settings_popup_visible(void)
{
    return g_view == VIEW_SETTINGS && g_settings_popup[0] && osGetTime() < g_settings_popup_until_ms;
}

static bool language_select_visible(void)
{
    return g_view == VIEW_SETTINGS && g_language_select_open;
}

#define LANGUAGE_SELECTOR_VISIBLE_ROWS 3

static void draw_settings_bottom_popup(void)
{
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COL_PAPER);
    C2D_DrawRectSolid(0, 0, 0, 320, 4, COL_PRIMARY);
    C2D_DrawRectSolid(18, 42, 0, 284, 122, 0xFF101010);
    C2D_DrawRectSolid(18, 42, 0, 284, 4, COL_SECONDARY);
    C2D_DrawRectSolid(18, 160, 0, 284, 4, COL_PRIMARY);
    draw_text_wrap(36, 64, 0.44f, 248, COL_WHITE, "%s", g_settings_popup);
    draw_text_wrap(36, 181, 0.35f, 248, COL_MUTED, "%s", tr(TR_POPUP_RESET_HELP));
}

static void draw_language_selector(void)
{
    if (!language_select_visible()) {
        return;
    }

    if (g_language_select_row < 0) {
        g_language_select_row = 0;
    }
    if (g_language_select_row >= LANG_COUNT) {
        g_language_select_row = LANG_COUNT - 1;
    }
    if (g_language_select_row < g_language_select_scroll) {
        g_language_select_scroll = g_language_select_row;
    }
    int visible_rows = LANG_COUNT < LANGUAGE_SELECTOR_VISIBLE_ROWS ? LANG_COUNT : LANGUAGE_SELECTOR_VISIBLE_ROWS;
    if (visible_rows < 1) {
        visible_rows = 1;
    }
    if (g_language_select_row >= g_language_select_scroll + visible_rows) {
        g_language_select_scroll = g_language_select_row - visible_rows + 1;
    }
    int max_scroll = LANG_COUNT - visible_rows;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (g_language_select_scroll > max_scroll) {
        g_language_select_scroll = max_scroll;
    }

    const float x = 82.0f;
    const float y = 62.0f;
    const float w = 236.0f;
    const float h = 116.0f;
    const float panel_z = 0.70f;
    const float text_z = 0.86f;
    C2D_DrawRectSolid(x, y, panel_z + 0.01f, w, h, COL_PAPER);
    C2D_DrawRectSolid(x, y, panel_z + 0.02f, w, 4, COL_SECONDARY);
    C2D_DrawRectSolid(x, y + h - 4, panel_z + 0.02f, w, 4, COL_PRIMARY);
    draw_text_centered_plain_z(x + w * 0.5f, y + 14, text_z, 0.42f, COL_WHITE, tr(TR_LANGUAGE_SELECT_TITLE));

    for (int row = 0; row < visible_rows && g_language_select_scroll + row < LANG_COUNT; row++) {
        int idx = g_language_select_scroll + row;
        bool selected = idx == g_language_select_row;
        float row_y = y + 43.0f + row * 25.0f;
        C2D_DrawRectSolid(x + 16, row_y - 3, panel_z + 0.03f, w - 32, 21, selected ? COL_PRIMARY_DARK : ((idx & 1) ? COL_CARD_2 : COL_CARD));
        C2D_DrawRectSolid(x + 16, row_y + 17, panel_z + 0.04f, w - 32, 1, selected ? COL_SECONDARY : 0x55000000);
        draw_text_plain_z(x + 28, row_y + 2, text_z, 0.40f, selected ? COL_WHITE : COL_MUTED, language_label(idx));
        if (idx == language_normalized(g_cfg.language)) {
            draw_text_plain_z(x + w - 58, row_y + 2, text_z, 0.34f, COL_SECONDARY, "*");
        }
    }
}

static const char *display_item_kind(const MediaItem *item)
{
    const char *kind = item && item->collection_type[0] ? item->collection_type : (item ? item->type : "");
    int language = language_normalized(g_cfg.language);
    if (language != LANG_ES && language != LANG_JA && language != LANG_FR && language != LANG_DE &&
        language != LANG_IT && language != LANG_RU) {
        return kind;
    }
    if (strcmp(kind, "movies") == 0 || strcmp(kind, "Movie") == 0) {
        switch (language) {
        case LANG_JA:
            return "映画";
        case LANG_FR:
            return "film";
        case LANG_DE:
            return "Film";
        case LANG_IT:
            return "film";
        case LANG_RU:
            return "фильм";
        default:
            return "pel" "\xC3\xAD" "cula";
        }
    }
    if (strcmp(kind, "tvshows") == 0 || strcmp(kind, "Series") == 0) {
        switch (language) {
        case LANG_JA:
            return "シリーズ";
        case LANG_FR:
            return "série";
        case LANG_DE:
            return "Serie";
        case LANG_IT:
            return "serie";
        case LANG_RU:
            return "сериал";
        default:
            return "serie";
        }
    }
    if (strcmp(kind, "Episode") == 0) {
        switch (language) {
        case LANG_JA:
            return "エピソード";
        case LANG_FR:
            return "épisode";
        case LANG_DE:
            return "Episode";
        case LANG_IT:
            return "episodio";
        case LANG_RU:
            return "эпизод";
        default:
            return "episodio";
        }
    }
    if (strcmp(kind, "Season") == 0) {
        switch (language) {
        case LANG_JA:
            return "シーズン";
        case LANG_FR:
            return "saison";
        case LANG_DE:
            return "Staffel";
        case LANG_IT:
            return "stagione";
        case LANG_RU:
            return "сезон";
        default:
            return "temporada";
        }
    }
    if (strcmp(kind, "Folder") == 0) {
        switch (language) {
        case LANG_JA:
            return "フォルダ";
        case LANG_FR:
            return "dossier";
        case LANG_DE:
            return "Ordner";
        case LANG_IT:
            return "cartella";
        case LANG_RU:
            return "папка";
        default:
            return "carpeta";
        }
    }
    if (strcmp(kind, "Video") == 0) {
        switch (language) {
        case LANG_JA:
            return "動画";
        case LANG_FR:
            return "vidéo";
        case LANG_DE:
            return "Video";
        case LANG_IT:
            return "video";
        case LANG_RU:
            return "видео";
        default:
            return "video";
        }
    }
    return kind;
}

static void draw_list(MediaItem *items, int count, const char *title)
{
    draw_header(title);

    if (count <= 0) {
        C2D_DrawRectSolid(24, 72, 0, 352, 86, COL_PAPER);
        draw_text(40, 88, 0.55f, COL_WHITE, "%s", tr(TR_NO_ITEMS_FOUND));
        draw_text_wrap(40, 116, 0.42f, 310, COL_MUTED, "%s", tr(TR_REFRESH_X_BACK_B));
        return;
    }

    if (g_selected < 0) {
        g_selected = 0;
    }
    if (g_selected >= count) {
        g_selected = count - 1;
    }
    if (g_selected < g_scroll) {
        g_scroll = g_selected;
    }
    if (g_selected >= g_scroll + 5) {
        g_scroll = g_selected - 4;
    }

    for (int row = 0; row < 5 && g_scroll + row < count; row++) {
        int idx = g_scroll + row;
        float y = 44.0f + row * 36.0f;
        bool sel = idx == g_selected;
        u32 card = sel ? COL_PRIMARY_DARK : ((idx & 1) ? COL_CARD_2 : COL_CARD);
        C2D_DrawRectSolid(16, y, 0, 368, 30, card);
        C2D_DrawRectSolid(16, y + 29, 0, 368, 1, sel ? COL_SECONDARY : 0x55000000);

        char name[72];
        clipped_name(items[idx].name, name, sizeof(name), 42);
        draw_text(25, y + 6, 0.43f, COL_WHITE, "%s", name);

        const char *kind = display_item_kind(&items[idx]);
        draw_text(290, y + 8, 0.35f, COL_MUTED, "%s", kind);
    }

    draw_text(18, 224, 0.36f, COL_MUTED, "%d/%d", g_selected + 1, count);
}

static void draw_setup(void)
{
    draw_header(tr(TR_SETUP));
    C2D_DrawRectSolid(18, 48, 0, 364, 146, COL_PAPER);

    const char *labels[] = {tr(TR_SERVER), tr(TR_USERNAME), tr(TR_PASSWORD), tr(TR_LOGIN)};
    char values[4][256];
    snprintf(values[0], sizeof(values[0]), "%s", g_cfg.server);
    snprintf(values[1], sizeof(values[1]), "%s", g_cfg.username[0] ? g_cfg.username : tr(TR_NOT_SET));
    snprintf(values[2], sizeof(values[2]), "%s", g_cfg.password[0] ? tr(TR_STORED) : tr(TR_NOT_SET));
    snprintf(values[3], sizeof(values[3]), "%s", g_cfg.token[0] ? tr(TR_REFRESH_TOKEN_SESSION) : tr(TR_AUTHENTICATE));
    QualityProfile q = quality_profile();
    char quality_label[16];
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);

    for (int i = 0; i < 4; i++) {
        float y = 58.0f + i * 32.0f;
        if (i == g_setup_row) {
            C2D_DrawRectSolid(28, y - 4, 0, 344, 24, COL_PRIMARY_DARK);
        }
        draw_text(36, y, 0.43f, COL_WHITE, "%s", labels[i]);
        draw_text(130, y, 0.39f, i == g_setup_row ? COL_WHITE : COL_MUTED, "%s", values[i]);
    }

    draw_text(28, 204, 0.34f, COL_MUTED, tr(TR_QUALITY_SETUP_FMT), quality_label, q.width, q.height);
    draw_text(28, 220, 0.34f, COL_MUTED, tr(TR_PLAYBACK_SETUP_FMT), g_is_new_3ds ? tr(TR_PLAYBACK_NEW3DS_LABEL) : tr(TR_PLAYBACK_OLD3DS_LABEL));
}

enum {
    SETTINGS_ROW_QUALITY,
    SETTINGS_ROW_PLAYBACK,
    SETTINGS_ROW_BUFFER,
    SETTINGS_ROW_AUDIO,
    SETTINGS_ROW_SERVER,
    SETTINGS_ROW_USERNAME,
    SETTINGS_ROW_CLEAR_LOGIN,
    SETTINGS_ROW_RESET_SERVER_USER,
    SETTINGS_ROW_RESET_DEFAULTS,
    SETTINGS_ROW_LANGUAGE,
    SETTINGS_ROW_COUNT
};

static void settings_value(int row, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return;
    }
    out[0] = 0;
    switch (row) {
    case SETTINGS_ROW_QUALITY:
        format_quality_label(out, outsz, g_cfg.quality);
        break;
    case SETTINGS_ROW_PLAYBACK:
        snprintf(out, outsz, "%s", playback_mode_label(g_cfg.playback_mode));
        break;
    case SETTINGS_ROW_BUFFER:
        snprintf(out, outsz, "%d KB", settings_buffer_normalized_kb(g_cfg.stream_buffer_kb));
        break;
    case SETTINGS_ROW_AUDIO:
        format_audio_rate_label(out, outsz, g_cfg.audio_sample_rate);
        break;
    case SETTINGS_ROW_SERVER:
        clipped_name(g_cfg.server[0] ? g_cfg.server : tr(TR_NOT_SET), out, outsz, 25);
        break;
    case SETTINGS_ROW_USERNAME:
        clipped_name(g_cfg.username[0] ? g_cfg.username : tr(TR_NOT_SET), out, outsz, 20);
        break;
    case SETTINGS_ROW_CLEAR_LOGIN:
        snprintf(out, outsz, "%s", (g_cfg.password[0] || g_cfg.token[0]) ? tr(TR_PASSWORD_TOKEN) : tr(TR_ALREADY_CLEAR));
        break;
    case SETTINGS_ROW_RESET_SERVER_USER:
        snprintf(out, outsz, "%s", tr(TR_SERVER_USER));
        break;
    case SETTINGS_ROW_RESET_DEFAULTS:
        snprintf(out, outsz, "%s", tr(TR_QUALITY_MODE_BUFFER));
        break;
    case SETTINGS_ROW_LANGUAGE:
        snprintf(out, outsz, "%s", language_label(g_cfg.language));
        break;
    default:
        snprintf(out, outsz, "-");
        break;
    }
}

static const char *settings_label(int row)
{
    switch (row) {
    case SETTINGS_ROW_QUALITY:
        return tr(TR_DEFAULT_QUALITY);
    case SETTINGS_ROW_PLAYBACK:
        return tr(TR_PLAYBACK_MODE);
    case SETTINGS_ROW_BUFFER:
        return tr(TR_STREAM_BUFFER);
    case SETTINGS_ROW_AUDIO:
        return tr(TR_AUDIO_RATE);
    case SETTINGS_ROW_SERVER:
        return tr(TR_SERVER_URL);
    case SETTINGS_ROW_USERNAME:
        return tr(TR_USERNAME);
    case SETTINGS_ROW_CLEAR_LOGIN:
        return tr(TR_CLEAR_SAVED_LOGIN);
    case SETTINGS_ROW_RESET_SERVER_USER:
        return tr(TR_RESET_SERVER_USER);
    case SETTINGS_ROW_RESET_DEFAULTS:
        return tr(TR_RESET_DEFAULTS);
    case SETTINGS_ROW_LANGUAGE:
        return tr(TR_LANGUAGE);
    default:
        return "";
    }
}

static void draw_settings(void)
{
    draw_header(tr(TR_SETTINGS));

    if (g_settings_row < 0) {
        g_settings_row = 0;
    }
    if (g_settings_row >= SETTINGS_ROW_COUNT) {
        g_settings_row = SETTINGS_ROW_COUNT - 1;
    }
    if (g_settings_row < g_settings_scroll) {
        g_settings_scroll = g_settings_row;
    }
    if (g_settings_row >= g_settings_scroll + 6) {
        g_settings_scroll = g_settings_row - 5;
    }

    C2D_DrawRectSolid(18, 43, 0, 364, 168, COL_PAPER);
    for (int row = 0; row < 6 && g_settings_scroll + row < SETTINGS_ROW_COUNT; row++) {
        int idx = g_settings_scroll + row;
        bool selected = idx == g_settings_row;
        float y = 49.0f + row * 27.0f;
        u32 fill = selected ? COL_PRIMARY_DARK : ((idx & 1) ? COL_CARD_2 : COL_CARD);
        C2D_DrawRectSolid(28, y - 3, 0, 344, 24, fill);
        C2D_DrawRectSolid(28, y + 20, 0, 344, 1, selected ? COL_SECONDARY : 0x55000000);
        char value[96];
        settings_value(idx, value, sizeof(value));
        draw_text(36, y + 2, 0.37f, COL_WHITE, "%s", settings_label(idx));
        draw_text(188, y + 2, 0.35f, selected ? COL_WHITE : COL_MUTED, "%s", value);
    }

    if (g_settings_row == SETTINGS_ROW_BUFFER) {
        draw_text_wrap(28, 217, 0.31f, 344, COL_MUTED,
                       "%s", tr(TR_BUFFER_NOTE));
    } else if (g_settings_row == SETTINGS_ROW_AUDIO) {
        int rate = settings_audio_sample_rate_normalized(g_cfg.audio_sample_rate);
        if (!g_is_new_3ds && rate == 40000) {
            draw_text_wrap(28, 217, 0.31f, 344, COL_SECONDARY,
                           "%s", tr(TR_AUDIO_40000_NOTE));
        } else {
            draw_text_wrap(28, 217, 0.31f, 344, COL_MUTED,
                           "%s", tr(TR_AUDIO_240_NOTE));
        }
    } else if (g_settings_row == SETTINGS_ROW_LANGUAGE) {
        draw_text_wrap(28, 217, 0.31f, 344, COL_MUTED,
                       "%s", tr(TR_LANGUAGE_NOTE));
    } else if (g_settings_confirm_row == g_settings_row && osGetTime() < g_settings_confirm_until_ms) {
        draw_text_wrap(28, 217, 0.31f, 344, COL_SECONDARY,
                       "%s", tr(TR_CONFIRM_AGAIN));
    } else {
        draw_text_wrap(28, 217, 0.31f, 344, COL_MUTED,
                       "%s", tr(TR_SETTINGS_HINT));
    }

    draw_language_selector();
}

static void draw_detail(void)
{
    char quality_label[16];
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
    draw_header(tr(TR_ITEM));
    C2D_DrawRectSolid(18, 50, 0, 364, 140, COL_PAPER);
    C2D_DrawRectSolid(18, 50, 0, 8, 140, COL_PRIMARY);
    draw_text_wrap(38, 64, 0.62f, 330, COL_WHITE, "%s", g_current.name);
    draw_text(38, 106, 0.43f, COL_MUTED, tr(TR_TYPE_FMT), display_item_kind(&g_current));
    if (g_current.year) {
        draw_text(38, 128, 0.43f, COL_MUTED, tr(TR_YEAR_FMT), g_current.year);
    }
    if (g_current.runtime_ticks) {
        unsigned long long minutes = g_current.runtime_ticks / 600000000ULL;
        draw_text(38, 150, 0.43f, COL_MUTED, tr(TR_RUNTIME_FMT), minutes);
    }
    draw_text(38, 174, 0.40f, COL_PRIMARY, tr(TR_REQUEST_TRANSCODE_FMT), quality_label);
}

static void draw_playback(void)
{
    draw_header(tr(TR_PLAYBACK));
    QualityProfile q = quality_profile();
    char quality_label[16];
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
    float preview_w = 110.0f + (float)q.height * 0.35f;
    if (preview_w > 250.0f) {
        preview_w = 250.0f;
    }
    C2D_DrawRectSolid(14, 45, 0, 372, 164, COL_PAPER);
    C2D_DrawRectSolid(24, 58, 0, preview_w, 74, COL_CARD);
    C2D_DrawRectSolid(24, 58, 0, (float)(g_frame_counter % (int)preview_w), 4, COL_PRIMARY);
    draw_text(36, 76, 0.55f, COL_WHITE, "%s", quality_label);
    draw_text(36, 103, 0.38f, COL_MUTED, "%s", g_play_method[0] ? g_play_method : tr(TR_STREAM));

    draw_text_wrap(220, 58, 0.38f, 150, COL_WHITE, "%s", g_current.name);
    draw_text_wrap(24, 145, 0.35f, 344, COL_MUTED, "%s", g_play_status[0] ? g_play_status : tr(TR_NO_PROBE_YET));
    draw_text_wrap(24, 178, 0.31f, 344, COL_MUTED, "%s", g_play_url[0] ? g_play_url : tr(TR_NO_URL));
}

static void render(void)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(g_text);

    C2D_TargetClear(g_top, COL_BG);
    C2D_SceneBegin(g_top);
    switch (g_view) {
    case VIEW_SETUP:
        draw_setup();
        break;
    case VIEW_SETTINGS:
        draw_settings();
        break;
    case VIEW_LIBRARIES:
        draw_list(g_libraries, g_library_count, tr(TR_LIBRARIES));
        break;
    case VIEW_ITEMS:
        draw_list(g_items, g_item_count, g_screen_title);
        break;
    case VIEW_DETAIL:
        draw_detail();
        break;
    case VIEW_PLAYBACK:
        draw_playback();
        break;
    }

    C2D_TargetClear(g_bottom, COL_PAPER);
    C2D_SceneBegin(g_bottom);
    if (settings_popup_visible()) {
        draw_settings_bottom_popup();
    } else if (language_select_visible()) {
        draw_bottom_help(tr(TR_LANGUAGE_SELECT_HELP),
                         "");
    } else if (g_view == VIEW_SETUP) {
        draw_bottom_help(tr(TR_SETUP_HELP_1),
                         tr(TR_SETUP_HELP_2));
    } else if (g_view == VIEW_SETTINGS) {
        draw_bottom_help(tr(TR_SETTINGS_HELP_1),
                         tr(TR_SETTINGS_HELP_2));
    } else if (g_view == VIEW_PLAYBACK) {
        draw_bottom_help(tr(TR_PLAYBACK_HELP_1),
                         tr(TR_PLAYBACK_HELP_2));
    } else if (g_view == VIEW_LIBRARIES) {
        draw_bottom_help(tr(TR_LIBRARIES_HELP_1),
                         "");
    } else {
        draw_bottom_help(tr(TR_ITEMS_HELP_1),
                         "");
    }

    C3D_FrameEnd(0);
}

static void ui_graphics_init(void)
{
    if (g_ui_ready) {
        return;
    }

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_text = C2D_TextBufNew(8192);
    g_ui_ready = true;
}

static void ui_graphics_exit(void)
{
    if (!g_ui_ready) {
        return;
    }

    if (g_text) {
        C2D_TextBufDelete(g_text);
        g_text = NULL;
    }
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    g_top = NULL;
    g_bottom = NULL;
    g_ui_ready = false;
}

static void move_selection(int delta, int count)
{
    if (count <= 0) {
        return;
    }
    g_selected += delta;
    if (g_selected < 0) {
        g_selected = count - 1;
    }
    if (g_selected >= count) {
        g_selected = 0;
    }
}

static void change_quality(int dir)
{
    int idx = quality_index(g_cfg.quality);
    if (idx < 0) {
        idx = quality_index(default_quality());
    }
    if (idx < 0) {
        idx = 0;
    }

    int count = 0;
    const int *levels = quality_levels(&count);
    idx = (idx + (dir >= 0 ? 1 : -1) + count) % count;
    g_cfg.quality = levels[idx];
    save_config();
    char quality_label[16];
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
    set_status(tr(TR_STATUS_QUALITY_TARGET_FMT), quality_label);
}

static void settings_show_popup(const char *message, u64 duration_ms)
{
    snprintf(g_settings_popup, sizeof(g_settings_popup), "%s", message ? message : "");
    g_settings_popup_until_ms = osGetTime() + duration_ms;
}

static void settings_cancel_confirm(void)
{
    g_settings_confirm_row = -1;
    g_settings_confirm_until_ms = 0;
    g_settings_popup[0] = 0;
    g_settings_popup_until_ms = 0;
    g_language_select_open = false;
}

static void open_settings(void)
{
    g_settings_return_view = g_view;
    g_view = VIEW_SETTINGS;
    settings_cancel_confirm();
    set_status("%s", tr(TR_STATUS_SETTINGS_OPENED));
}

static bool settings_confirm_action(int row, const char *message)
{
    u64 now = osGetTime();
    if (g_settings_confirm_row == row && now < g_settings_confirm_until_ms) {
        settings_cancel_confirm();
        return true;
    }
    g_settings_confirm_row = row;
    g_settings_confirm_until_ms = now + 3000;
    settings_show_popup(message, 3000);
    set_status("%s", message);
    return false;
}

static void change_playback_mode(int dir)
{
    int mode = is_supported_playback_mode(g_cfg.playback_mode) ? g_cfg.playback_mode : PLAYBACK_MODE_AUTO;
    int count = PLAYBACK_MODE_MJPEG + 1;
    mode = (mode + (dir >= 0 ? 1 : -1) + count) % count;
    g_cfg.playback_mode = mode;
    save_config();
    settings_cancel_confirm();
    set_status(tr(TR_STATUS_PLAYBACK_MODE_FMT), playback_mode_label(g_cfg.playback_mode));
}

static void change_buffer_preference(int dir)
{
    int kb = settings_buffer_normalized_kb(g_cfg.stream_buffer_kb);
    kb += (dir >= 0 ? SETTINGS_BUFFER_KB_STEP : -SETTINGS_BUFFER_KB_STEP);
    if (kb > SETTINGS_BUFFER_KB_MAX) {
        kb = SETTINGS_BUFFER_KB_MIN;
    } else if (kb < SETTINGS_BUFFER_KB_MIN) {
        kb = SETTINGS_BUFFER_KB_MAX;
    }
    g_cfg.stream_buffer_kb = kb;
    save_config();
    settings_cancel_confirm();
    if (kb > SETTINGS_BUFFER_KB_DEFAULT) {
        set_status(tr(TR_STATUS_BUFFER_HIGH_FMT), kb);
    } else if (kb < SETTINGS_BUFFER_KB_DEFAULT) {
        set_status(tr(TR_STATUS_BUFFER_LOW_FMT), kb);
    } else {
        set_status(tr(TR_STATUS_BUFFER_STABLE_FMT), kb);
    }
}

static void change_audio_rate_setting(int dir)
{
    int count = 0;
    const int *levels = audio_rate_levels(&count);
    if (count <= 0) {
        return;
    }
    int idx = audio_rate_index(g_cfg.audio_sample_rate);
    if (idx < 0) {
        idx = audio_rate_index(default_audio_sample_rate());
    }
    if (idx < 0) {
        idx = 0;
    }
    idx = (idx + (dir >= 0 ? 1 : -1) + count) % count;
    g_cfg.audio_sample_rate = levels[idx];
    save_config();
    settings_cancel_confirm();
    if (!g_is_new_3ds && g_cfg.audio_sample_rate == 40000) {
        set_status("%s", tr(TR_STATUS_AUDIO_40000));
    } else {
        set_status(tr(TR_STATUS_AUDIO_FMT), g_cfg.audio_sample_rate);
    }
}

static void set_language_setting(int language)
{
    g_cfg.language = language_normalized(language);
    save_config();
    settings_cancel_confirm();
    set_status(tr(TR_STATUS_LANGUAGE_FMT), language_label(g_cfg.language));
}

static void open_language_selector(void)
{
    int language = language_normalized(g_cfg.language);
    settings_cancel_confirm();
    g_language_select_row = language;
    g_language_select_scroll = language >= LANGUAGE_SELECTOR_VISIBLE_ROWS ? language - LANGUAGE_SELECTOR_VISIBLE_ROWS + 1 : 0;
    g_language_select_open = true;
    set_status("%s", tr(TR_LANGUAGE_SELECT_TITLE));
}

static bool handle_language_selector(u32 down)
{
    if (!g_language_select_open) {
        return false;
    }
    if (down & KEY_DOWN) {
        g_language_select_row = (g_language_select_row + 1) % LANG_COUNT;
    }
    if (down & KEY_UP) {
        g_language_select_row = (g_language_select_row + LANG_COUNT - 1) % LANG_COUNT;
    }
    if (down & KEY_B) {
        g_language_select_open = false;
        set_status("%s", tr(TR_STATUS_PROMPT_CLOSED));
        return true;
    }
    if (down & KEY_A) {
        set_language_setting(g_language_select_row);
        return true;
    }
    return true;
}

static void settings_clear_saved_login(void)
{
    g_cfg.password[0] = 0;
    g_cfg.token[0] = 0;
    g_cfg.user_id[0] = 0;
    g_play_session[0] = 0;
    g_library_count = 0;
    g_item_count = 0;
    save_config();
    set_status("%s", tr(TR_STATUS_LOGIN_CLEARED));
}

static void settings_reset_server_user(void)
{
    g_cfg.server[0] = 0;
    g_cfg.username[0] = 0;
    g_cfg.password[0] = 0;
    g_cfg.token[0] = 0;
    g_cfg.user_id[0] = 0;
    g_play_session[0] = 0;
    g_library_count = 0;
    g_item_count = 0;
    save_config();
    set_status("%s", tr(TR_STATUS_SERVER_USER_RESET));
}

static void settings_reset_defaults(void)
{
    g_cfg.quality = default_quality();
    g_cfg.playback_mode = PLAYBACK_MODE_AUTO;
    g_cfg.stream_buffer_kb = SETTINGS_BUFFER_KB_DEFAULT;
    g_cfg.audio_sample_rate = default_audio_sample_rate();
    save_config();
    char quality_label[16];
    format_quality_label(quality_label, sizeof(quality_label), g_cfg.quality);
    set_status(tr(TR_STATUS_DEFAULTS_FMT), quality_label, g_cfg.stream_buffer_kb, g_cfg.audio_sample_rate);
}

static void handle_settings(u32 down)
{
    if (handle_language_selector(down)) {
        return;
    }

    if (down & KEY_DOWN) {
        g_settings_row = (g_settings_row + 1) % SETTINGS_ROW_COUNT;
        settings_cancel_confirm();
    }
    if (down & KEY_UP) {
        g_settings_row = (g_settings_row + SETTINGS_ROW_COUNT - 1) % SETTINGS_ROW_COUNT;
        settings_cancel_confirm();
    }
    if (down & KEY_B) {
        if (settings_popup_visible()) {
            settings_cancel_confirm();
            set_status("%s", tr(TR_STATUS_PROMPT_CLOSED));
            return;
        }
        settings_cancel_confirm();
        g_view = (!g_cfg.token[0] || !g_cfg.user_id[0]) ? VIEW_SETUP : g_settings_return_view;
        set_status("%s", tr(TR_STATUS_SETTINGS_CLOSED));
        return;
    }

    int dir = 0;
    if (down & KEY_DLEFT) {
        dir = -1;
    } else if (down & KEY_DRIGHT) {
        dir = 1;
    }
    if (dir != 0) {
        if (g_settings_row == SETTINGS_ROW_QUALITY) {
            change_quality(dir);
        } else if (g_settings_row == SETTINGS_ROW_PLAYBACK) {
            change_playback_mode(dir);
        } else if (g_settings_row == SETTINGS_ROW_BUFFER) {
            change_buffer_preference(dir);
        } else if (g_settings_row == SETTINGS_ROW_AUDIO) {
            change_audio_rate_setting(dir);
        } else if (g_settings_row == SETTINGS_ROW_LANGUAGE) {
            open_language_selector();
        }
    }

    if (!(down & KEY_A)) {
        return;
    }

    switch (g_settings_row) {
    case SETTINGS_ROW_QUALITY:
        change_quality(1);
        break;
    case SETTINGS_ROW_PLAYBACK:
        change_playback_mode(1);
        break;
    case SETTINGS_ROW_BUFFER:
        change_buffer_preference(1);
        break;
    case SETTINGS_ROW_AUDIO:
        change_audio_rate_setting(1);
        break;
    case SETTINGS_ROW_SERVER: {
        char old_server[sizeof(g_cfg.server)];
        copy_safe(old_server, sizeof(old_server), g_cfg.server);
        if (edit_text(tr(TR_SERVER_URL), g_cfg.server, sizeof(g_cfg.server), false)) {
            normalize_server_url(g_cfg.server);
            if (strcmp(old_server, g_cfg.server) != 0) {
                g_cfg.token[0] = 0;
                g_cfg.user_id[0] = 0;
            }
            save_config();
            set_status("%s", tr(TR_STATUS_SERVER_SAVED));
        }
        settings_cancel_confirm();
        break;
    }
    case SETTINGS_ROW_USERNAME: {
        char old_username[sizeof(g_cfg.username)];
        copy_safe(old_username, sizeof(old_username), g_cfg.username);
        if (edit_text(tr(TR_USERNAME), g_cfg.username, sizeof(g_cfg.username), false)) {
            if (strcmp(old_username, g_cfg.username) != 0) {
                g_cfg.token[0] = 0;
                g_cfg.user_id[0] = 0;
            }
            save_config();
            set_status("%s", tr(TR_STATUS_USERNAME_SAVED));
        }
        settings_cancel_confirm();
        break;
    }
    case SETTINGS_ROW_CLEAR_LOGIN:
        if (settings_confirm_action(g_settings_row, tr(TR_CONFIRM_CLEAR_LOGIN))) {
            settings_clear_saved_login();
        }
        break;
    case SETTINGS_ROW_RESET_SERVER_USER:
        if (settings_confirm_action(g_settings_row, tr(TR_CONFIRM_RESET_SERVER))) {
            settings_reset_server_user();
        }
        break;
    case SETTINGS_ROW_RESET_DEFAULTS:
        if (settings_confirm_action(g_settings_row, tr(TR_CONFIRM_RESET_DEFAULTS))) {
            settings_reset_defaults();
        }
        break;
    case SETTINGS_ROW_LANGUAGE:
        open_language_selector();
        break;
    default:
        break;
    }
}

static void handle_setup(u32 down)
{
    if (down & KEY_DOWN) {
        g_setup_row = (g_setup_row + 1) % 4;
    }
    if (down & KEY_UP) {
        g_setup_row = (g_setup_row + 3) % 4;
    }
    if (down & KEY_B) {
        if (g_cfg.token[0] && g_cfg.user_id[0]) {
            load_libraries();
        }
    }
    if (down & KEY_A) {
        if (g_setup_row == 0 && edit_text(tr(TR_SERVER_URL), g_cfg.server, sizeof(g_cfg.server), false)) {
            normalize_server_url(g_cfg.server);
            save_config();
        } else if (g_setup_row == 1 && edit_text(tr(TR_USERNAME), g_cfg.username, sizeof(g_cfg.username), false)) {
            g_cfg.token[0] = 0;
            g_cfg.user_id[0] = 0;
            save_config();
        } else if (g_setup_row == 2 && edit_text(tr(TR_PASSWORD), g_cfg.password, sizeof(g_cfg.password), true)) {
            g_cfg.token[0] = 0;
            g_cfg.user_id[0] = 0;
            save_config();
        } else if (g_setup_row == 3) {
            if (login_jellyfin()) {
                load_libraries();
            }
        }
    }
}

static void handle_common(u32 down)
{
    (void)down;
}

static void handle_input(u32 down)
{
    if (g_view == VIEW_SETUP) {
        handle_setup(down);
        return;
    }
    if (g_view == VIEW_SETTINGS) {
        handle_settings(down);
        return;
    }

    handle_common(down);

    if (g_view == VIEW_LIBRARIES) {
        if (down & KEY_DOWN) {
            move_selection(1, g_library_count);
        }
        if (down & KEY_UP) {
            move_selection(-1, g_library_count);
        }
        if (down & KEY_X) {
            load_libraries();
        }
        if (down & KEY_Y) {
            open_settings();
        }
        if (down & KEY_B) {
            g_view = VIEW_SETUP;
            set_status("%s", tr(TR_SETUP_OPENED));
        }
        if ((down & KEY_A) && g_library_count > 0) {
            push_nav("", tr(TR_LIBRARIES));
            load_items_for_parent(g_libraries[g_selected].id, g_libraries[g_selected].name);
        }
    } else if (g_view == VIEW_ITEMS) {
        if (down & KEY_DOWN) {
            move_selection(1, g_item_count);
        }
        if (down & KEY_UP) {
            move_selection(-1, g_item_count);
        }
        if (down & KEY_X) {
            if (g_current_parent_id[0]) {
                load_items_for_parent(g_current_parent_id, g_screen_title);
            }
        }
        if (down & KEY_B) {
            pop_nav();
        }
        if (down & KEY_Y) {
            open_settings();
        }
        if ((down & KEY_A) && g_item_count > 0) {
            MediaItem *item = &g_items[g_selected];
            if (is_playable(item)) {
                g_current = *item;
                g_return_view = VIEW_ITEMS;
                probe_playback();
            } else {
                push_nav(g_current_parent_id, g_screen_title);
                load_items_for_parent(item->id, item->name);
            }
        }
    } else if (g_view == VIEW_DETAIL) {
        if (down & KEY_B) {
            g_view = VIEW_ITEMS;
        }
        if (down & KEY_Y) {
            open_settings();
        }
        if (down & KEY_A) {
            g_return_view = VIEW_DETAIL;
            probe_playback();
        }
    } else if (g_view == VIEW_PLAYBACK) {
        if (down & KEY_B) {
            g_view = g_return_view;
        }
        if (down & KEY_X) {
            probe_playback();
        }
    }
}
