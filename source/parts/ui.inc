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

static bool ui_codepoint_is_hangul(u32 cp)
{
    return (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0x3130 && cp <= 0x318F) ||
           (cp >= 0x1100 && cp <= 0x11FF);
}

static bool ui_codepoint_is_cjk_ideograph(u32 cp)
{
    return (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF);
}

static bool ui_codepoint_is_kana(u32 cp)
{
    return (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0x31F0 && cp <= 0x31FF);
}

static bool ui_text_has_hangul(const char *text)
{
    const char *p = text ? text : "";
    const char *end = p + strlen(p);
    while (p < end) {
        u32 cp = 0;
        size_t len = read_utf8_codepoint(p, end, &cp);
        if (len == 0) {
            p++;
            continue;
        }
        if (ui_codepoint_is_hangul(cp)) {
            return true;
        }
        p += len;
    }
    return false;
}

static bool ui_text_has_cjk_ideograph(const char *text)
{
    const char *p = text ? text : "";
    const char *end = p + strlen(p);
    while (p < end) {
        u32 cp = 0;
        size_t len = read_utf8_codepoint(p, end, &cp);
        if (len == 0) {
            p++;
            continue;
        }
        if (ui_codepoint_is_cjk_ideograph(cp)) {
            return true;
        }
        p += len;
    }
    return false;
}

static bool ui_text_has_kana(const char *text)
{
    const char *p = text ? text : "";
    const char *end = p + strlen(p);
    while (p < end) {
        u32 cp = 0;
        size_t len = read_utf8_codepoint(p, end, &cp);
        if (len == 0) {
            p++;
            continue;
        }
        if (ui_codepoint_is_kana(cp)) {
            return true;
        }
        p += len;
    }
    return false;
}

static void ui_text_parse(C2D_Text *text, const char *src)
{
    const char *safe = src ? src : "";
    if (g_font_kor && ui_text_has_hangul(safe)) {
        C2D_TextFontParse(text, g_font_kor, g_text, safe);
    } else if ((g_font_chn || g_font_twn) &&
               (language_normalized(g_cfg.language) == LANG_ZH_HANS ||
                language_normalized(g_cfg.language) == LANG_ZH_HANT ||
                strcmp(safe, language_label(LANG_ZH_HANS)) == 0 ||
                strcmp(safe, language_label(LANG_ZH_HANT)) == 0) &&
               strcmp(safe, language_label(LANG_JA)) != 0 &&
               ui_text_has_cjk_ideograph(safe) && !ui_text_has_kana(safe)) {
        bool hant_text = language_normalized(g_cfg.language) == LANG_ZH_HANT ||
                         strcmp(safe, language_label(LANG_ZH_HANT)) == 0;
        C2D_Font font = hant_text && g_font_twn ? g_font_twn : g_font_chn;
        if (!font) {
            font = g_font_twn;
        }
        C2D_TextFontParse(text, font, g_text, safe);
    } else {
        C2D_TextParse(text, g_text, safe);
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
    ui_text_parse(&t, buf);
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
    ui_text_parse(&t, buf);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap, x, y, 0.5f, scale, scale, color, wrap);
}

static float text_width_for_scale(const char *text, float scale)
{
    C2D_Text t;
    ui_text_parse(&t, text ? text : "");
    C2D_TextOptimize(&t);

    float width = 0.0f;
    float height = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &width, &height);
    (void)height;
    return width;
}

static int utf8_char_bytes(const char *s)
{
    unsigned char c = s && s[0] ? (unsigned char)s[0] : 0;
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static void trim_text_to_width(char *text, size_t textsz, float scale, float wrap, bool ellipsis)
{
    if (!text || textsz == 0) {
        return;
    }

    while (text[0] && text_width_for_scale(text, scale) > wrap) {
        size_t len = strlen(text);
        if (len == 0) {
            break;
        }
        do {
            text[--len] = 0;
        } while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80);
    }

    while (strlen(text) > 0 && text[strlen(text) - 1] == ' ') {
        text[strlen(text) - 1] = 0;
    }

    if (ellipsis && text[0]) {
        while (strlen(text) + 3 >= textsz) {
            size_t len = strlen(text);
            if (len == 0) {
                break;
            }
            do {
                text[--len] = 0;
            } while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80);
        }
        strncat(text, "...", textsz - strlen(text) - 1);
        while (text_width_for_scale(text, scale) > wrap && strlen(text) > 3) {
            size_t len = strlen(text) - 3;
            text[len] = 0;
            do {
                text[--len] = 0;
            } while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80);
            strncat(text, "...", textsz - strlen(text) - 1);
        }
    }
}

static void draw_text_lines(float x, float y, float scale, float wrap, u32 color,
                            const char *text, int max_lines, float line_step)
{
    if (!text || !text[0] || max_lines <= 0) {
        return;
    }

    const char *p = text;
    for (int line = 0; line < max_lines; line++) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        while (*p == '\n') {
            p++;
        }
        if (!*p) {
            break;
        }

        char out[192];
        out[0] = 0;
        const char *line_start = p;
        bool forced_newline = false;

        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\r') {
                p++;
            }
            if (*p == '\n') {
                p++;
                forced_newline = true;
                break;
            }
            if (!*p) {
                break;
            }

            const char *word_start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                p += utf8_char_bytes(p);
            }
            size_t word_len = (size_t)(p - word_start);

            char candidate[192];
            if (out[0]) {
                snprintf(candidate, sizeof(candidate), "%s %.*s", out, (int)word_len, word_start);
            } else {
                snprintf(candidate, sizeof(candidate), "%.*s", (int)word_len, word_start);
            }

            if (text_width_for_scale(candidate, scale) <= wrap || !out[0]) {
                copy_safe(out, sizeof(out), candidate);
                if (text_width_for_scale(out, scale) > wrap) {
                    trim_text_to_width(out, sizeof(out), scale, wrap, line < max_lines - 1 || *p);
                    break;
                }
            } else {
                p = word_start;
                break;
            }
        }

        if (!out[0] && p > line_start) {
            snprintf(out, sizeof(out), "%.*s", (int)(p - line_start), line_start);
            trim_text_to_width(out, sizeof(out), scale, wrap, line < max_lines - 1 || *p);
        }

        if (line == max_lines - 1) {
            const char *rest = p;
            while (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\n') {
                rest++;
            }
            if (*rest) {
                trim_text_to_width(out, sizeof(out), scale, wrap, true);
            }
        }

        draw_text(x, y + (float)line * line_step, scale, color, "%s", out);

        if (forced_newline) {
            continue;
        }
    }
}

static void draw_text_centered(float center_x, float y, float scale, u32 color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    C2D_Text t;
    ui_text_parse(&t, buf);
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
    ui_text_parse(&t, text ? text : "");
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, z, scale, scale, color);
}

static void draw_text_centered_plain_z(float center_x, float y, float z, float scale, u32 color, const char *text)
{
    C2D_Text t;
    ui_text_parse(&t, text ? text : "");
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

    const float x = 38.0f;
    const float y = 35.0f;
    const float w = 324.0f;
    const float h = 178.0f;
    const float row_x = x + 30.0f;
    const float row_y0 = y + 58.0f;
    const float row_w = 244.0f;
    const float row_h = 25.0f;
    const float row_gap = 7.0f;
    const float rail_x = x + w - 28.0f;
    const float rail_y = row_y0 - 1.0f;
    const float rail_h = visible_rows * row_h + (visible_rows - 1) * row_gap + 2.0f;
    const float panel_z = 0.70f;
    const float text_z = 0.86f;
    C2D_DrawRectSolid(0, 35, panel_z, 400, 205, 0xAA090B0F);
    C2D_DrawRectSolid(x + 5.0f, y + 6.0f, panel_z + 0.01f, w, h, 0x99000000);
    C2D_DrawRectSolid(x, y, panel_z + 0.02f, w, h, 0xFF171A20);
    C2D_DrawRectSolid(x, y, panel_z + 0.03f, w, 1, 0xFF4E5663);
    C2D_DrawRectSolid(x, y + h - 1.0f, panel_z + 0.03f, w, 1, 0xFF07080B);
    C2D_DrawRectSolid(x, y, panel_z + 0.03f, 1, h, 0xFF4E5663);
    C2D_DrawRectSolid(x + w - 1.0f, y, panel_z + 0.03f, 1, h, 0xFF07080B);
    C2D_DrawRectSolid(x, y, panel_z + 0.04f, w, 4, COL_SECONDARY);
    C2D_DrawRectSolid(x, y + 4.0f, panel_z + 0.04f, w, 2, COL_PRIMARY);
    C2D_DrawRectSolid(x + 18.0f, y + 48.0f, panel_z + 0.04f, w - 36.0f, 1, 0xFF3B4450);

    draw_text_plain_z(x + 22.0f, y + 13.0f, text_z, 0.56f, COL_WHITE, tr(TR_LANGUAGE_SELECT_TITLE));
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", g_language_select_row + 1, LANG_COUNT);
    draw_text_plain_z(x + w - 60.0f, y + 17.0f, text_z, 0.42f, COL_MUTED, counter);

    C2D_DrawRectSolid(rail_x, rail_y, panel_z + 0.04f, 4, rail_h, 0xFF303742);
    float thumb_h = ((float)visible_rows / (float)LANG_COUNT) * rail_h;
    if (thumb_h < 17.0f) {
        thumb_h = 17.0f;
    }
    if (thumb_h > rail_h) {
        thumb_h = rail_h;
    }
    float thumb_y = rail_y;
    if (max_scroll > 0) {
        thumb_y += ((rail_h - thumb_h) * (float)g_language_select_scroll) / (float)max_scroll;
    }
    C2D_DrawRectSolid(rail_x, thumb_y, panel_z + 0.05f, 4, thumb_h, COL_PRIMARY);
    if (g_language_select_scroll > 0) {
        draw_text_centered_plain_z(rail_x + 2.0f, rail_y - 12.0f, text_z, 0.25f, COL_SECONDARY, "^");
    }
    if (g_language_select_scroll < max_scroll) {
        draw_text_centered_plain_z(rail_x + 2.0f, rail_y + rail_h - 9.0f, text_z, 0.25f, COL_SECONDARY, "v");
    }

    for (int row = 0; row < visible_rows && g_language_select_scroll + row < LANG_COUNT; row++) {
        int idx = g_language_select_scroll + row;
        bool selected = idx == g_language_select_row;
        bool current = idx == language_normalized(g_cfg.language);
        float row_y = row_y0 + row * (row_h + row_gap);
        u32 fill = selected ? COL_PRIMARY_DARK : (current ? 0xFF22313A : ((idx & 1) ? 0xFF1C2E35 : 0xFF18252C));
        C2D_DrawRectSolid(row_x + 2.0f, row_y + 2.0f, panel_z + 0.03f, row_w, row_h, 0x66000000);
        C2D_DrawRectSolid(row_x, row_y, panel_z + 0.04f, row_w, row_h, fill);
        C2D_DrawRectSolid(row_x, row_y, panel_z + 0.05f, 4, row_h, selected ? COL_SECONDARY : (current ? COL_PRIMARY : 0xFF48515D));
        C2D_DrawRectSolid(row_x, row_y, panel_z + 0.05f, row_w, 1, selected ? 0xFF63DFF0 : 0xFF34404A);
        C2D_DrawRectSolid(row_x, row_y + row_h - 1.0f, panel_z + 0.05f, row_w, 1, selected ? COL_SECONDARY : 0xFF0C1015);
        draw_text_plain_z(row_x + 16.0f, row_y + 5.0f, text_z, 0.40f, selected ? COL_WHITE : COL_MUTED, language_label(idx));
        if (current) {
            C2D_DrawRectSolid(row_x + row_w - 19.0f, row_y + 8.0f, panel_z + 0.06f, 8, 8, COL_SECONDARY);
            C2D_DrawRectSolid(row_x + row_w - 17.0f, row_y + 10.0f, panel_z + 0.07f, 4, 4, COL_WHITE);
        }
    }

}

static const char *display_item_kind(const MediaItem *item)
{
    const char *kind = item && item->collection_type[0] ? item->collection_type : (item ? item->type : "");
    int language = language_normalized(g_cfg.language);
    if (language == LANG_EN) {
        if (strcmp(kind, "tvshows") == 0) {
            return "TV Shows";
        }
        if (strcmp(kind, "movies") == 0) {
            return "Movies";
        }
        if (strcmp(kind, "homevideos") == 0) {
            return "Home Videos";
        }
        if (strcmp(kind, "music") == 0) {
            return "Music";
        }
        if (strcmp(kind, "books") == 0) {
            return "Books";
        }
    }
    if (language != LANG_AF && language != LANG_ES && language != LANG_JA && language != LANG_FR && language != LANG_DE &&
        language != LANG_IT && language != LANG_RU && language != LANG_NL && language != LANG_PT &&
        language != LANG_PL && language != LANG_ID && language != LANG_KO && language != LANG_ZH_HANS && language != LANG_ZH_HANT) {
        return kind;
    }
    if (strcmp(kind, "movies") == 0 || strcmp(kind, "Movie") == 0) {
        switch (language) {
        case LANG_JA:
            return "映画";
        case LANG_KO:
            return "영화";
        case LANG_ZH_HANS:
            return "电影";
        case LANG_ZH_HANT:
            return "電影";
        case LANG_FR:
            return "film";
        case LANG_DE:
            return "Film";
        case LANG_IT:
            return "film";
        case LANG_RU:
            return "фильм";
        case LANG_NL:
            return "film";
        case LANG_PT:
            return "filme";
        case LANG_PL:
            return "film";
        case LANG_ID:
            return "film";
        case LANG_AF:
            return "film";
        default:
            return "pel" "\xC3\xAD" "cula";
        }
    }
    if (strcmp(kind, "tvshows") == 0 || strcmp(kind, "Series") == 0) {
        switch (language) {
        case LANG_JA:
            return "シリーズ";
        case LANG_KO:
            return "시리즈";
        case LANG_ZH_HANS:
            return "剧集";
        case LANG_ZH_HANT:
            return "劇集";
        case LANG_FR:
            return "série";
        case LANG_DE:
            return "Serie";
        case LANG_IT:
            return "serie";
        case LANG_RU:
            return "сериал";
        case LANG_NL:
            return "serie";
        case LANG_PT:
            return "série";
        case LANG_PL:
            return "serial";
        case LANG_ID:
            return "serial";
        case LANG_AF:
            return "reeks";
        default:
            return "serie";
        }
    }
    if (strcmp(kind, "Episode") == 0) {
        switch (language) {
        case LANG_JA:
            return "エピソード";
        case LANG_KO:
            return "에피소드";
        case LANG_ZH_HANS:
            return "集";
        case LANG_ZH_HANT:
            return "集";
        case LANG_FR:
            return "épisode";
        case LANG_DE:
            return "Episode";
        case LANG_IT:
            return "episodio";
        case LANG_RU:
            return "эпизод";
        case LANG_NL:
            return "aflevering";
        case LANG_PT:
            return "episódio";
        case LANG_PL:
            return "odcinek";
        case LANG_ID:
            return "episode";
        case LANG_AF:
            return "episode";
        default:
            return "episodio";
        }
    }
    if (strcmp(kind, "Season") == 0) {
        switch (language) {
        case LANG_JA:
            return "シーズン";
        case LANG_KO:
            return "시즌";
        case LANG_ZH_HANS:
            return "季";
        case LANG_ZH_HANT:
            return "季";
        case LANG_FR:
            return "saison";
        case LANG_DE:
            return "Staffel";
        case LANG_IT:
            return "stagione";
        case LANG_RU:
            return "сезон";
        case LANG_NL:
            return "seizoen";
        case LANG_PT:
            return "temporada";
        case LANG_PL:
            return "sezon";
        case LANG_ID:
            return "musim";
        case LANG_AF:
            return "seisoen";
        default:
            return "temporada";
        }
    }
    if (strcmp(kind, "Folder") == 0) {
        switch (language) {
        case LANG_JA:
            return "フォルダ";
        case LANG_KO:
            return "폴더";
        case LANG_ZH_HANS:
            return "文件夹";
        case LANG_ZH_HANT:
            return "資料夾";
        case LANG_FR:
            return "dossier";
        case LANG_DE:
            return "Ordner";
        case LANG_IT:
            return "cartella";
        case LANG_RU:
            return "папка";
        case LANG_NL:
            return "map";
        case LANG_PT:
            return "pasta";
        case LANG_PL:
            return "folder";
        case LANG_ID:
            return "folder";
        case LANG_AF:
            return "vouer";
        default:
            return "carpeta";
        }
    }
    if (strcmp(kind, "Video") == 0) {
        switch (language) {
        case LANG_JA:
            return "動画";
        case LANG_KO:
            return "동영상";
        case LANG_ZH_HANS:
            return "视频";
        case LANG_ZH_HANT:
            return "影片";
        case LANG_FR:
            return "vidéo";
        case LANG_DE:
            return "Video";
        case LANG_IT:
            return "video";
        case LANG_RU:
            return "видео";
        case LANG_NL:
            return "video";
        case LANG_PT:
            return "vídeo";
        case LANG_PL:
            return "wideo";
        case LANG_ID:
            return "video";
        case LANG_AF:
            return "video";
        default:
            return "video";
        }
    }
    return kind;
}

enum {
    BROWSE_VISIBLE_ROWS = 5,
    BROWSE_LANDSCAPE_ART_W = 184,
    BROWSE_LANDSCAPE_ART_H = 104,
    BROWSE_POSTER_ART_W = 112,
    BROWSE_POSTER_ART_H = 160,
    BROWSE_ART_TEX_W = 256,
    BROWSE_ART_TEX_H = 256
};

static C3D_Tex g_browse_art_tex;
static Tex3DS_SubTexture g_browse_art_subtex;
static bool g_browse_art_tex_ready;
static bool g_browse_art_loaded;
static bool g_browse_art_failed;
static int g_browse_art_w;
static int g_browse_art_h;
static char g_browse_art_id[80];

static const u8 BROWSE_TILE_REMAP[64] = {
    0x00, 0x01, 0x08, 0x09, 0x02, 0x03, 0x0A, 0x0B,
    0x10, 0x11, 0x18, 0x19, 0x12, 0x13, 0x1A, 0x1B,
    0x04, 0x05, 0x0C, 0x0D, 0x06, 0x07, 0x0E, 0x0F,
    0x14, 0x15, 0x1C, 0x1D, 0x16, 0x17, 0x1E, 0x1F,
    0x20, 0x21, 0x28, 0x29, 0x22, 0x23, 0x2A, 0x2B,
    0x30, 0x31, 0x38, 0x39, 0x32, 0x33, 0x3A, 0x3B,
    0x24, 0x25, 0x2C, 0x2D, 0x26, 0x27, 0x2E, 0x2F,
    0x34, 0x35, 0x3C, 0x3D, 0x36, 0x37, 0x3E, 0x3F
};

static u16 browse_texture_pixel(u16 rgb565)
{
    return rgb565;
}

static void browse_swizzle_rgb565(const u16 *linear, u16 *tiled, int width, int height)
{
    if (!linear || !tiled || width <= 0 || height <= 0) {
        return;
    }

    int write = 0;
    for (int ty = 0; ty < height; ty += 8) {
        for (int tx = 0; tx < width; tx += 8) {
            for (int i = 0; i < 64; i++) {
                int idx = BROWSE_TILE_REMAP[i];
                int sx = idx & 7;
                int sy = idx >> 3;
                tiled[write++] = browse_texture_pixel(linear[(ty + sy) * width + tx + sx]);
            }
        }
    }
}

static void clamp_browse_selection(int count, int visible_rows)
{
    if (count <= 0) {
        g_selected = 0;
        g_scroll = 0;
        return;
    }
    if (visible_rows < 1) {
        visible_rows = 1;
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
    if (g_selected >= g_scroll + visible_rows) {
        g_scroll = g_selected - visible_rows + 1;
    }

    int max_scroll = count - visible_rows;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (g_scroll > max_scroll) {
        g_scroll = max_scroll;
    }
    if (g_scroll < 0) {
        g_scroll = 0;
    }
}

static bool browse_item_prefers_poster_art(const MediaItem *item)
{
    if (!item) {
        return false;
    }
    return strcmp(item->type, "Movie") == 0 ||
           strcmp(item->type, "Series") == 0 ||
           strcmp(item->type, "Season") == 0 ||
           strcmp(item->type, "BoxSet") == 0;
}

static void browse_art_target_size(const MediaItem *item, int *out_w, int *out_h)
{
    if (browse_item_prefers_poster_art(item)) {
        *out_w = BROWSE_POSTER_ART_W;
        *out_h = BROWSE_POSTER_ART_H;
    } else {
        *out_w = BROWSE_LANDSCAPE_ART_W;
        *out_h = BROWSE_LANDSCAPE_ART_H;
    }
}

static void browse_set_art_size(int w, int h)
{
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    if (w > BROWSE_ART_TEX_W) {
        w = BROWSE_ART_TEX_W;
    }
    if (h > BROWSE_ART_TEX_H) {
        h = BROWSE_ART_TEX_H;
    }

    g_browse_art_w = w;
    g_browse_art_h = h;
    g_browse_art_subtex.width = (u16)w;
    g_browse_art_subtex.height = (u16)h;
    g_browse_art_subtex.left = 0.0f;
    g_browse_art_subtex.right = (float)w / (float)BROWSE_ART_TEX_W;
    g_browse_art_subtex.top = 1.0f;
    g_browse_art_subtex.bottom = 1.0f - ((float)h / (float)BROWSE_ART_TEX_H);
}

static bool browse_art_texture_init(void)
{
    if (g_browse_art_tex_ready) {
        return true;
    }

    memset(&g_browse_art_tex, 0, sizeof(g_browse_art_tex));
    if (!C3D_TexInit(&g_browse_art_tex, BROWSE_ART_TEX_W, BROWSE_ART_TEX_H, GPU_RGB565)) {
        return false;
    }
    C3D_TexSetFilter(&g_browse_art_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_browse_art_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    browse_set_art_size(BROWSE_LANDSCAPE_ART_W, BROWSE_LANDSCAPE_ART_H);

    g_browse_art_tex_ready = true;
    return true;
}

static void browse_art_texture_free(void)
{
    if (g_browse_art_tex_ready) {
        C3D_TexDelete(&g_browse_art_tex);
        memset(&g_browse_art_tex, 0, sizeof(g_browse_art_tex));
        g_browse_art_tex_ready = false;
    }
    g_browse_art_loaded = false;
    g_browse_art_failed = false;
    g_browse_art_w = 0;
    g_browse_art_h = 0;
    g_browse_art_id[0] = 0;
}

static void browse_fill_fallback_art(u16 *pixels, int view_w, int view_h)
{
    if (!pixels) {
        return;
    }
    u16 bg = rgb565_from_rgb(15, 20, 27);
    u16 line = rgb565_from_rgb(0, 164, 220);
    u16 line2 = rgb565_from_rgb(170, 92, 195);
    u16 pad = rgb565_from_rgb(10, 13, 18);
    for (int y = 0; y < BROWSE_ART_TEX_H; y++) {
        for (int x = 0; x < BROWSE_ART_TEX_W; x++) {
            u16 color = pad;
            if (x < view_w && y < view_h) {
                color = bg;
            }
            if (y < 4 && x < view_w) {
                color = line;
            } else if (y >= view_h - 4 && y < view_h && x < view_w) {
                color = line2;
            } else if (x < view_w && y < view_h && ((x + y) & 31) < 2) {
                color = rgb565_from_rgb(28, 39, 50);
            }
            pixels[y * BROWSE_ART_TEX_W + x] = color;
        }
    }
}

static bool browse_scale_into_art_texture(u16 *tex_pixels, const u16 *src, int src_w, int src_h,
                                          int max_w, int max_h, bool fill_frame)
{
    if (!tex_pixels || !src || src_w <= 0 || src_h <= 0) {
        return false;
    }

    u16 bg = rgb565_from_rgb(10, 13, 18);
    for (int i = 0; i < BROWSE_ART_TEX_W * BROWSE_ART_TEX_H; i++) {
        tex_pixels[i] = bg;
    }

    int dst_w = max_w;
    int dst_h = max_h;
    int dst_x0 = 0;
    int dst_y0 = 0;
    int crop_x0 = 0;
    int crop_y0 = 0;
    int crop_w = src_w;
    int crop_h = src_h;

    if (fill_frame) {
        if ((long long)src_w * max_h > (long long)src_h * max_w) {
            crop_w = (src_h * max_w) / max_h;
            if (crop_w < 1) {
                crop_w = 1;
            }
            crop_x0 = (src_w - crop_w) / 2;
        } else if ((long long)src_h * max_w > (long long)src_w * max_h) {
            crop_h = (src_w * max_h) / max_w;
            if (crop_h < 1) {
                crop_h = 1;
            }
            crop_y0 = (src_h - crop_h) / 2;
        }
    } else {
        dst_h = (src_h * dst_w) / src_w;
        if (dst_h > max_h) {
            dst_h = max_h;
            dst_w = (src_w * dst_h) / src_h;
        }
        dst_x0 = (max_w - dst_w) / 2;
        dst_y0 = (max_h - dst_h) / 2;
    }

    if (dst_w < 1) {
        dst_w = 1;
    }
    if (dst_h < 1) {
        dst_h = 1;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = crop_y0 + (y * crop_h) / dst_h;
        if (sy >= src_h) {
            sy = src_h - 1;
        }
        for (int x = 0; x < dst_w; x++) {
            int sx = crop_x0 + (x * crop_w) / dst_w;
            if (sx >= src_w) {
                sx = src_w - 1;
            }
            tex_pixels[(dst_y0 + y) * BROWSE_ART_TEX_W + dst_x0 + x] = src[sy * src_w + sx];
        }
    }
    return true;
}

static void browse_extrude_art_edges(u16 *tex_pixels, int view_w, int view_h)
{
    if (!tex_pixels || view_w <= 0 || view_h <= 0 ||
        view_w > BROWSE_ART_TEX_W || view_h > BROWSE_ART_TEX_H) {
        return;
    }

    if (view_w < BROWSE_ART_TEX_W) {
        for (int y = 0; y < view_h; y++) {
            u16 edge = tex_pixels[y * BROWSE_ART_TEX_W + view_w - 1];
            tex_pixels[y * BROWSE_ART_TEX_W + view_w] = edge;
        }
    }
    if (view_h < BROWSE_ART_TEX_H) {
        u16 *last = tex_pixels + (view_h - 1) * BROWSE_ART_TEX_W;
        u16 *next = tex_pixels + view_h * BROWSE_ART_TEX_W;
        for (int x = 0; x < view_w; x++) {
            next[x] = last[x];
        }
        if (view_w < BROWSE_ART_TEX_W) {
            next[view_w] = last[view_w - 1];
        }
    }
}

static bool browse_fetch_primary_art(const MediaItem *item)
{
    if (!item || !item->id[0] || !g_cfg.server[0] || !g_cfg.token[0]) {
        return false;
    }
    if (!browse_art_texture_init()) {
        return false;
    }

    u16 *decoded = (u16 *)malloc(JPEG_PIXELS_CAP * sizeof(u16));
    u16 *tex_pixels = (u16 *)malloc(BROWSE_ART_TEX_W * BROWSE_ART_TEX_H * sizeof(u16));
    u16 *tiled_pixels = (u16 *)malloc(BROWSE_ART_TEX_W * BROWSE_ART_TEX_H * sizeof(u16));
    if (!decoded || !tex_pixels || !tiled_pixels) {
        free(decoded);
        free(tex_pixels);
        free(tiled_pixels);
        return false;
    }

    char enc_id[160];
    char path[320];
    int max_w = 0;
    int max_h = 0;
    browse_art_target_size(item, &max_w, &max_h);
    browse_set_art_size(max_w, max_h);
    url_encode(item->id, enc_id, sizeof(enc_id));
    snprintf(path, sizeof(path),
             "/Items/%s/Images/Primary?fillWidth=%d&fillHeight=%d&quality=90&format=jpg",
             enc_id, max_w, max_h);

    bool ok = false;
    HttpResponse res;
    Result ret = api_get(path, &res);
    if (R_SUCCEEDED(ret) && res.status >= 200 && res.status < 300 && res.body && res.size > 0) {
        int img_w = 0;
        int img_h = 0;
        unsigned jpg_status = 0;
        if (decode_jpeg_rgb565((const u8 *)res.body, res.size, decoded, &img_w, &img_h, &jpg_status)) {
            if (browse_scale_into_art_texture(tex_pixels, decoded, img_w, img_h,
                                              max_w, max_h, true)) {
                browse_extrude_art_edges(tex_pixels, max_w, max_h);
                browse_set_art_size(max_w, max_h);
                browse_swizzle_rgb565(tex_pixels, tiled_pixels, BROWSE_ART_TEX_W, BROWSE_ART_TEX_H);
                C3D_TexUpload(&g_browse_art_tex, tiled_pixels);
                C3D_TexFlush(&g_browse_art_tex);
                ok = true;
            }
        }
    }
    free_response(&res);

    free(decoded);
    free(tex_pixels);
    free(tiled_pixels);
    return ok;
}

static void browse_ensure_art_for_item(const MediaItem *item)
{
    const char *id = item ? item->id : "";
    if (strcmp(g_browse_art_id, id) == 0) {
        return;
    }

    copy_safe(g_browse_art_id, sizeof(g_browse_art_id), id);
    g_browse_art_loaded = false;
    g_browse_art_failed = false;

    if (browse_fetch_primary_art(item)) {
        g_browse_art_loaded = true;
    } else {
        u16 *tex_pixels = (u16 *)malloc(BROWSE_ART_TEX_W * BROWSE_ART_TEX_H * sizeof(u16));
        u16 *tiled_pixels = (u16 *)malloc(BROWSE_ART_TEX_W * BROWSE_ART_TEX_H * sizeof(u16));
        int max_w = 0;
        int max_h = 0;
        browse_art_target_size(item, &max_w, &max_h);
        browse_set_art_size(max_w, max_h);
        if (tex_pixels && tiled_pixels && browse_art_texture_init()) {
            browse_fill_fallback_art(tex_pixels, max_w, max_h);
            browse_swizzle_rgb565(tex_pixels, tiled_pixels, BROWSE_ART_TEX_W, BROWSE_ART_TEX_H);
            C3D_TexUpload(&g_browse_art_tex, tiled_pixels);
            C3D_TexFlush(&g_browse_art_tex);
        }
        free(tex_pixels);
        free(tiled_pixels);
        g_browse_art_failed = true;
    }
}

static void browse_format_meta(const MediaItem *item, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return;
    }
    out[0] = 0;
    if (!item) {
        return;
    }

    if (strcmp(item->type, "Episode") == 0 && item->parent_index_number >= 0 && item->index_number >= 0) {
        snprintf(out, outsz, "S%d:E%d", item->parent_index_number, item->index_number);
    } else if (strcmp(item->type, "Season") == 0 && item->index_number >= 0) {
        snprintf(out, outsz, "%s %d", display_item_kind(item), item->index_number);
    } else if (item->year > 0) {
        snprintf(out, outsz, "%d", item->year);
    }

    if (item->runtime_ticks) {
        unsigned long long minutes = item->runtime_ticks / 600000000ULL;
        char runtime[32];
        snprintf(runtime, sizeof(runtime), "%llu min", minutes);
        if (out[0]) {
            strncat(out, "  /  ", outsz - strlen(out) - 1);
        }
        strncat(out, runtime, outsz - strlen(out) - 1);
    }

    if (!out[0]) {
        snprintf(out, outsz, "%s", display_item_kind(item));
    }
}

static void draw_browse_art(float x, float y)
{
    int art_w = g_browse_art_w > 0 ? g_browse_art_w : BROWSE_LANDSCAPE_ART_W;
    int art_h = g_browse_art_h > 0 ? g_browse_art_h : BROWSE_LANDSCAPE_ART_H;
    C2D_DrawRectSolid(x - 2, y - 2, 0, art_w + 4, art_h + 4, 0xFF07090C);
    C2D_DrawRectSolid(x - 1, y - 1, 0, art_w + 2, art_h + 2, 0xFF2A323B);
    if (g_browse_art_tex_ready) {
        C2D_Image img = {&g_browse_art_tex, &g_browse_art_subtex};
        C2D_DrawImageAt(img, x, y, 0.1f, NULL, 1.0f, 1.0f);
    } else {
        C2D_DrawRectSolid(x, y, 0, art_w, art_h, 0xFF121820);
    }
    if (g_browse_art_failed) {
        C2D_DrawRectSolid(x, y + art_h - 16, 0, art_w, 16, 0xCC101820);
        draw_text_centered(x + art_w * 0.5f, y + art_h - 13.0f, 0.25f, COL_MUTED, "%s", "No image");
    }
}

static void draw_browse_preview(MediaItem *items, int count, const char *title)
{
    draw_header(title);

    if (count <= 0) {
        C2D_DrawRectSolid(24, 72, 0, 352, 86, COL_PAPER);
        draw_text(40, 88, 0.55f, COL_WHITE, "%s", tr(TR_NO_ITEMS_FOUND));
        draw_text_wrap(40, 116, 0.42f, 310, COL_MUTED, "%s", tr(TR_REFRESH_X_BACK_B));
        return;
    }

    clamp_browse_selection(count, BROWSE_VISIBLE_ROWS);

    MediaItem *item = &items[g_selected];

    char name[128];
    char meta[96];
    char kind[48];
    clipped_name(item->name, name, sizeof(name), 58);
    browse_format_meta(item, meta, sizeof(meta));
    clipped_name(display_item_kind(item), kind, sizeof(kind), 24);

    bool poster_art = browse_item_prefers_poster_art(item);
    float art_x = poster_art ? 18.0f : 14.0f;
    float art_y = poster_art ? 47.0f : 52.0f;
    float reserved_art_w = poster_art ? (float)BROWSE_POSTER_ART_W : (float)BROWSE_LANDSCAPE_ART_W;
    float text_x = art_x + reserved_art_w + 22.0f;
    float text_w = 388.0f - text_x;
    if (text_w < 120.0f) {
        text_w = 120.0f;
    }
    float meta_y = poster_art ? 106.0f : 112.0f;
    float badge_y = poster_art ? 128.0f : 133.0f;
    float overview_x = poster_art ? text_x : 24.0f;
    float overview_y = poster_art ? 163.0f : 164.0f;
    float overview_w = poster_art ? text_w : 352.0f;

    C2D_DrawRectSolid(0, 35, 0, 400, 205, 0xFF0D1014);
    C2D_DrawRectSolid(0, 35, 0, 400, 181, 0xFF171C22);
    if (!poster_art) {
        C2D_DrawRectSolid(0, 157, 0, 400, 59, 0xFF11161A);
    }
    C2D_DrawRectSolid(0, 216, 0, 400, 1, 0xFF303842);

    draw_browse_art(art_x, art_y);

    draw_text_lines(text_x, 54, poster_art ? 0.51f : 0.57f, text_w, COL_WHITE,
                    name, 2, poster_art ? 18.0f : 20.0f);
    draw_text(text_x, meta_y, 0.39f, COL_MUTED, "%s", meta);

    C2D_DrawRectSolid(text_x, badge_y, 0, 102, 21, COL_PRIMARY_DARK);
    C2D_DrawRectSolid(text_x, badge_y + 20, 0, 102, 1, COL_SECONDARY);
    draw_text(text_x + 8, badge_y + 4, 0.33f, COL_WHITE, "%s", kind);

    if (item->media_source_count > 1) {
        draw_text(text_x + 112, badge_y + 5, 0.29f, COL_MUTED, "%d src", item->media_source_count);
    } else if (item->is_missing || item->is_virtual_item || item->is_place_holder) {
        draw_text(text_x + 112, badge_y + 5, 0.29f, COL_SECONDARY, "%s", "virtual");
    }

    char position[24];
    snprintf(position, sizeof(position), "%d/%d", g_selected + 1, count);
    C2D_DrawRectSolid(330, badge_y, 0, 50, 21, 0xFF242A32);
    draw_text_centered(355.0f, badge_y + 5, 0.29f, COL_MUTED, "%s", position);

    if (item->overview[0]) {
        draw_text_lines(overview_x, overview_y, 0.35f, overview_w, COL_MUTED,
                        item->overview, 3, 14.5f);
    }
}

static void browse_preload_selected_art(void)
{
    MediaItem *items = NULL;
    int count = 0;

    if (g_view == VIEW_LIBRARIES) {
        items = g_libraries;
        count = g_library_count;
    } else if (g_view == VIEW_ITEMS || g_view == VIEW_SEARCH) {
        items = g_items;
        count = g_item_count;
    } else {
        return;
    }

    if (count <= 0) {
        return;
    }

    clamp_browse_selection(count, BROWSE_VISIBLE_ROWS);
    browse_ensure_art_for_item(&items[g_selected]);
}

static void draw_browse_bottom_list(MediaItem *items, int count, const char *title, const char *help)
{
    C2D_DrawRectSolid(0, 0, 0, 320, 240, 0xFF12161B);
    C2D_DrawRectSolid(0, 0, 0, 320, 28, 0xFF1E232A);
    C2D_DrawRectSolid(0, 28, 0, 320, 2, COL_SECONDARY);

    char short_title[48];
    clipped_name(title ? title : "", short_title, sizeof(short_title), 22);
    draw_text(12, 7, 0.35f, COL_WHITE, "%s", short_title);

    if (count <= 0) {
        draw_text_wrap(18, 52, 0.43f, 284, COL_WHITE, "%s", tr(TR_NO_ITEMS_FOUND));
        draw_text_wrap(18, 84, 0.34f, 284, COL_MUTED, "%s", tr(TR_REFRESH_X_BACK_B));
        return;
    }

    clamp_browse_selection(count, BROWSE_VISIBLE_ROWS);

    char counter[24];
    snprintf(counter, sizeof(counter), "%d/%d", g_selected + 1, count);
    draw_text(270, 8, 0.32f, COL_MUTED, "%s", counter);

    for (int row = 0; row < BROWSE_VISIBLE_ROWS && g_scroll + row < count; row++) {
        int idx = g_scroll + row;
        float y = 38.0f + row * 32.0f;
        bool sel = idx == g_selected;
        u32 fill = sel ? 0xFF0B6386 : ((idx & 1) ? 0xFF1B2730 : 0xFF172129);
        C2D_DrawRectSolid(10, y, 0, 300, 27, fill);
        C2D_DrawRectSolid(10, y, 0, 4, 27, sel ? COL_SECONDARY : 0xFF39414B);
        C2D_DrawRectSolid(10, y + 26, 0, 300, 1, sel ? COL_SECONDARY : 0x55000000);

        char row_name[72];
        char row_kind[36];
        clipped_name(items[idx].name, row_name, sizeof(row_name), 28);
        clipped_name(display_item_kind(&items[idx]), row_kind, sizeof(row_kind), 12);
        draw_text(21, y + 6, 0.36f, sel ? COL_WHITE : COL_MUTED, "%s", row_name);
        draw_text(240, y + 8, 0.29f, sel ? COL_WHITE : COL_MUTED, "%s", row_kind);
    }

    if (count > BROWSE_VISIBLE_ROWS) {
        float rail_y = 38.0f;
        float rail_h = 155.0f;
        int max_scroll = count - BROWSE_VISIBLE_ROWS;
        float thumb_h = ((float)BROWSE_VISIBLE_ROWS / (float)count) * rail_h;
        if (thumb_h < 18.0f) {
            thumb_h = 18.0f;
        }
        float thumb_y = rail_y + (max_scroll > 0 ? ((float)g_scroll / (float)max_scroll) * (rail_h - thumb_h) : 0.0f);
        C2D_DrawRectSolid(314, rail_y, 0, 3, rail_h, 0xFF303844);
        C2D_DrawRectSolid(314, thumb_y, 0, 3, thumb_h, COL_PRIMARY);
    }

    C2D_DrawRectSolid(0, 204, 0, 320, 36, 0xFF1B2027);
    C2D_DrawRectSolid(0, 204, 0, 320, 1, COL_PRIMARY);
    char help_buf[192];
    if (g_view == VIEW_LIBRARIES || g_view == VIEW_ITEMS) {
        snprintf(help_buf, sizeof(help_buf), "%s  SELECT %s", help ? help : "", tr(TR_SEARCH));
    } else {
        snprintf(help_buf, sizeof(help_buf), "%s", help ? help : "");
    }
    draw_text_wrap(12, 212, 0.29f, 296, COL_MUTED, "%s", help_buf);
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
    SETTINGS_ROW_BOTTOM_DIM,
    SETTINGS_ROW_SERVER,
    SETTINGS_ROW_USERNAME,
    SETTINGS_ROW_CLEAR_LOGIN,
    SETTINGS_ROW_RESET_SERVER_USER,
    SETTINGS_ROW_RESET_DEFAULTS,
    SETTINGS_ROW_LANGUAGE,
    SETTINGS_ROW_COUNT
};

static void format_bottom_dim_label(char *out, size_t outsz, int seconds)
{
    if (!out || outsz == 0) {
        return;
    }
    seconds = settings_bottom_dim_normalized_seconds(seconds);
    if (seconds == 0) {
        snprintf(out, outsz, "%s", tr(TR_BOTTOM_DIM_NEVER));
    } else if (seconds < 60) {
        snprintf(out, outsz, tr(TR_TIME_SECONDS_FMT), seconds);
    } else {
        snprintf(out, outsz, tr(TR_TIME_MINUTES_FMT), seconds / 60);
    }
}

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
    case SETTINGS_ROW_BOTTOM_DIM:
        format_bottom_dim_label(out, outsz, g_cfg.bottom_dim_seconds);
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
    case SETTINGS_ROW_BOTTOM_DIM:
        return tr(TR_BOTTOM_DIM);
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
    } else if (g_settings_row == SETTINGS_ROW_BOTTOM_DIM) {
        draw_text_wrap(28, 217, 0.31f, 344, COL_MUTED,
                       "%s", tr(TR_BOTTOM_DIM_NOTE));
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
    browse_preload_selected_art();

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
        draw_browse_preview(g_libraries, g_library_count, tr(TR_LIBRARIES));
        break;
    case VIEW_ITEMS:
        draw_browse_preview(g_items, g_item_count, g_screen_title);
        break;
    case VIEW_SEARCH:
        draw_browse_preview(g_items, g_item_count, g_screen_title);
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
        draw_browse_bottom_list(g_libraries, g_library_count, tr(TR_LIBRARIES),
                                tr(TR_LIBRARIES_HELP_1));
    } else if (g_view == VIEW_SEARCH) {
        draw_browse_bottom_list(g_items, g_item_count, g_screen_title,
                                tr(TR_SEARCH_HELP_1));
    } else {
        draw_browse_bottom_list(g_items, g_item_count, g_screen_title,
                                tr(TR_ITEMS_HELP_1));
    }

    C3D_FrameEnd(0);
}

static void ui_graphics_init(void)
{
    if (g_ui_ready) {
        return;
    }

    gfxInitDefault();
    g_cfgu_ready = R_SUCCEEDED(cfguInit());
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_text = C2D_TextBufNew(8192);
    if (g_cfgu_ready) {
        g_font_kor = C2D_FontLoadSystem(CFG_REGION_KOR);
        g_font_chn = C2D_FontLoadSystem(CFG_REGION_CHN);
        g_font_twn = C2D_FontLoadSystem(CFG_REGION_TWN);
    }
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
    if (g_font_kor) {
        C2D_FontFree(g_font_kor);
        g_font_kor = NULL;
    }
    if (g_font_chn) {
        C2D_FontFree(g_font_chn);
        g_font_chn = NULL;
    }
    if (g_font_twn) {
        C2D_FontFree(g_font_twn);
        g_font_twn = NULL;
    }
    browse_art_texture_free();
    C2D_Fini();
    C3D_Fini();
    if (g_cfgu_ready) {
        cfguExit();
        g_cfgu_ready = false;
    }
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

enum {
    NAV_REPEAT_START_MS = 320,
    NAV_REPEAT_INTERVAL_MS = 72
};

static int g_nav_repeat_dir;
static View g_nav_repeat_view;
static u64 g_nav_repeat_next_ms;

static int nav_dir_from_keys(u32 keys)
{
    bool down = (keys & KEY_DOWN) != 0;
    bool up = (keys & KEY_UP) != 0;
    if (down == up) {
        return 0;
    }
    return down ? 1 : -1;
}

static void reset_nav_repeat(void)
{
    g_nav_repeat_dir = 0;
    g_nav_repeat_next_ms = 0;
}

static int repeated_nav_delta(u32 down, u32 held)
{
    int pressed_dir = nav_dir_from_keys(down);
    u64 now = osGetTime();
    if (pressed_dir != 0) {
        g_nav_repeat_dir = pressed_dir;
        g_nav_repeat_view = g_view;
        g_nav_repeat_next_ms = now + NAV_REPEAT_START_MS;
        return pressed_dir;
    }

    int held_dir = nav_dir_from_keys(held);
    if (held_dir == 0 || held_dir != g_nav_repeat_dir || g_nav_repeat_view != g_view) {
        reset_nav_repeat();
        return 0;
    }

    if (now < g_nav_repeat_next_ms) {
        return 0;
    }

    g_nav_repeat_next_ms = now + NAV_REPEAT_INTERVAL_MS;
    return held_dir;
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

static void change_bottom_dim_setting(int dir)
{
    int count = (int)(sizeof(BOTTOM_DIM_LEVELS) / sizeof(BOTTOM_DIM_LEVELS[0]));
    int idx = bottom_dim_index(settings_bottom_dim_normalized_seconds(g_cfg.bottom_dim_seconds));
    if (idx < 0) {
        idx = bottom_dim_index(BOTTOM_DIM_DEFAULT_SECONDS);
    }
    if (idx < 0) {
        idx = 0;
    }
    idx = (idx + (dir >= 0 ? 1 : -1) + count) % count;
    g_cfg.bottom_dim_seconds = BOTTOM_DIM_LEVELS[idx];
    save_config();
    settings_cancel_confirm();

    char label[24];
    format_bottom_dim_label(label, sizeof(label), g_cfg.bottom_dim_seconds);
    if (g_cfg.bottom_dim_seconds == 0) {
        set_status("%s", tr(TR_STATUS_BOTTOM_DIM_NEVER));
    } else {
        set_status(tr(TR_STATUS_BOTTOM_DIM_FMT), label);
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
    g_cfg.bottom_dim_seconds = BOTTOM_DIM_DEFAULT_SECONDS;
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
        } else if (g_settings_row == SETTINGS_ROW_BOTTOM_DIM) {
            change_bottom_dim_setting(dir);
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
    case SETTINGS_ROW_BOTTOM_DIM:
        change_bottom_dim_setting(1);
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

static void capture_search_return(void)
{
    if (g_view == VIEW_SEARCH) {
        return;
    }

    g_search_return_view = g_view;
    copy_safe(g_search_return_parent_id, sizeof(g_search_return_parent_id), g_current_parent_id);
    copy_safe(g_search_return_title, sizeof(g_search_return_title), g_screen_title);
    g_search_return_selected = g_selected;
    g_search_return_scroll = g_scroll;
}

static void return_from_search(void)
{
    reset_nav_repeat();
    if (g_search_return_view == VIEW_ITEMS) {
        load_items_for_parent(g_search_return_parent_id, g_search_return_title);
        g_selected = g_search_return_selected;
        g_scroll = g_search_return_scroll;
        clamp_browse_selection(g_item_count, BROWSE_VISIBLE_ROWS);
    } else {
        load_libraries();
        g_selected = g_search_return_selected;
        g_scroll = g_search_return_scroll;
        clamp_browse_selection(g_library_count, BROWSE_VISIBLE_ROWS);
    }
}

static void open_search_keyboard(bool capture_return)
{
    if (capture_return) {
        capture_search_return();
    }

    char query[sizeof(g_search_query)];
    copy_safe(query, sizeof(query), g_search_query);
    if (!edit_text(tr(TR_SEARCH_PROMPT), query, sizeof(query), false)) {
        set_status("%s", tr(TR_STATUS_SEARCH_CANCELLED));
        return;
    }

    if (!query[0]) {
        set_status("%s", tr(TR_STATUS_SEARCH_CANCELLED));
        return;
    }

    load_search_results(query);
}

static void handle_input(u32 down, u32 held)
{
    if (g_view == VIEW_SETUP) {
        reset_nav_repeat();
        handle_setup(down);
        return;
    }
    if (g_view == VIEW_SETTINGS) {
        reset_nav_repeat();
        handle_settings(down);
        return;
    }

    handle_common(down);

    if (g_view == VIEW_LIBRARIES) {
        int nav_delta = repeated_nav_delta(down, held);
        if (nav_delta != 0) {
            move_selection(nav_delta, g_library_count);
        }
        if (down & KEY_X) {
            load_libraries();
        }
        if (down & KEY_Y) {
            open_settings();
        }
        if (down & KEY_SELECT) {
            open_search_keyboard(true);
            return;
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
        int nav_delta = repeated_nav_delta(down, held);
        if (nav_delta != 0) {
            move_selection(nav_delta, g_item_count);
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
        if (down & KEY_SELECT) {
            open_search_keyboard(true);
            return;
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
    } else if (g_view == VIEW_SEARCH) {
        int nav_delta = repeated_nav_delta(down, held);
        if (nav_delta != 0) {
            move_selection(nav_delta, g_item_count);
        }
        if (down & KEY_X) {
            open_search_keyboard(false);
            return;
        }
        if (down & KEY_SELECT) {
            open_search_keyboard(false);
            return;
        }
        if (down & KEY_B) {
            return_from_search();
            return;
        }
        if (down & KEY_Y) {
            open_settings();
            return;
        }
        if ((down & KEY_A) && g_item_count > 0) {
            MediaItem *item = &g_items[g_selected];
            if (is_playable(item)) {
                g_current = *item;
                g_return_view = VIEW_SEARCH;
                probe_playback();
            } else {
                push_nav(SEARCH_NAV_PARENT_ID, g_screen_title);
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
        reset_nav_repeat();
        if (down & KEY_B) {
            g_view = g_return_view;
        }
        if (down & KEY_X) {
            probe_playback();
        }
    }
}
