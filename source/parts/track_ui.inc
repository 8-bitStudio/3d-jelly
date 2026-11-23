/* Subtitle overlay and the audio/subtitle picker.
 *
 * During video the screens are raw framebuffers, so citro2d can't draw text.
 * Instead glyphs are sampled in software straight from the console's system
 * font sheets: proper lowercase, accents, Cyrillic and Japanese at full screen
 * resolution, independent of the video quality. A subtitle is rasterised once
 * when it changes and cached; each video frame only blends the cached pixels. */

static void bottom_fill_rect(u8 *fb, int x, int y, int w, int h, u8 r, u8 g, u8 b);
static void bottom_stroke_rect(u8 *fb, int x, int y, int w, int h, u8 r, u8 g, u8 b);
static void bottom_draw_text(u8 *fb, int x, int y, const char *text, int scale, u8 r, u8 g, u8 b);
static int bottom_text_width(const char *text, int scale);
static void fb_draw_text_stroked(u16 *fb, int x, int y, const char *text, int scale, u16 color, int alpha);

typedef void (*SysPlotFn)(void *ctx, int x, int y, int alpha);

typedef struct {
    C2D_Font font; /* NULL = system font */
    const FINF_s *finf;
    int glyph;
} SysGlyph;

static bool g_sysfont_checked;
static bool g_sysfont_ok;
static u8 g_sysfont_morton[64];

static bool sysfont_info_usable(const FINF_s *finf)
{
    const TGLP_s *t = finf ? finf->tglp : NULL;
    return t && t->sheetData && t->cellHeight > 0 && t->sheetWidth >= 8 && t->sheetHeight >= 8 &&
           (t->sheetFmt == GPU_A4 || t->sheetFmt == GPU_A8);
}

static bool sysfont_ready(void)
{
    if (!g_sysfont_checked) {
        g_sysfont_checked = true;
        /* Index inside an 8x8 tile: x and y bits interleaved (Z order). */
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                g_sysfont_morton[y * 8 + x] = (u8)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                                                   ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
            }
        }
        g_sysfont_ok = R_SUCCEEDED(fontEnsureMapped()) && sysfont_info_usable(C2D_FontGetInfo(NULL));
    }
    return g_sysfont_ok;
}

static float sysfont_scale_for_px(float px)
{
    const FINF_s *finf = C2D_FontGetInfo(NULL);
    return px / (float)finf->tglp->cellHeight;
}

/* Finds a glyph, falling back to the Korean/Chinese system fonts when the
 * UI already loaded them (loading one here would stall the video). */
static void sysfont_lookup(u32 cp, SysGlyph *out)
{
    const FINF_s *sys = C2D_FontGetInfo(NULL);
    out->font = NULL;
    out->finf = sys;
    out->glyph = C2D_FontGlyphIndexFromCodePoint(NULL, cp);
    if (out->glyph != sys->alterCharIndex || cp < 0x80) {
        return;
    }
    C2D_Font alts[3] = { g_font_kor, g_font_chn, g_font_twn };
    for (int i = 0; i < 3; i++) {
        if (!alts[i]) {
            continue;
        }
        const FINF_s *finf = C2D_FontGetInfo(alts[i]);
        if (!sysfont_info_usable(finf)) {
            continue;
        }
        int glyph = C2D_FontGlyphIndexFromCodePoint(alts[i], cp);
        if (glyph != finf->alterCharIndex) {
            out->font = alts[i];
            out->finf = finf;
            out->glyph = glyph;
            return;
        }
    }
}

/* Fallback fonts are sized to match the system font's cell height. */
static float sysfont_glyph_scale(const SysGlyph *g, float scale)
{
    if (!g->font) {
        return scale;
    }
    const FINF_s *sys = C2D_FontGetInfo(NULL);
    return scale * (float)sys->tglp->cellHeight / (float)g->finf->tglp->cellHeight;
}

static float sysfont_advance(u32 cp, float scale)
{
    SysGlyph g;
    sysfont_lookup(cp, &g);
    charWidthInfo_s *cwi = C2D_FontGetCharWidthInfo(g.font, g.glyph);
    int width = cwi ? cwi->charWidth : g.finf->defaultWidth.charWidth;
    return (float)width * sysfont_glyph_scale(&g, scale);
}

static inline int sysfont_sheet_alpha(const TGLP_s *t, const u8 *sheet, int x, int y)
{
    if (x < 0 || y < 0 || x >= t->sheetWidth || y >= t->sheetHeight) {
        return 0;
    }
    u32 p = ((((u32)y >> 3) * ((u32)t->sheetWidth >> 3) + ((u32)x >> 3)) << 6) |
            g_sysfont_morton[((y & 7) << 3) | (x & 7)];
    if (t->sheetFmt == GPU_A4) {
        u8 b = sheet[p >> 1];
        return ((p & 1) ? (b >> 4) : (b & 0x0F)) * 17;
    }
    return sheet[p];
}

/* Draws one glyph with its cell top at y; returns the pen advance. */
static float sysfont_draw_glyph(u32 cp, float x, int y, float scale, SysPlotFn plot, void *ctx)
{
    SysGlyph g;
    sysfont_lookup(cp, &g);
    float gs = sysfont_glyph_scale(&g, scale);
    charWidthInfo_s *cwi = C2D_FontGetCharWidthInfo(g.font, g.glyph);
    int glyph_w = cwi ? cwi->glyphWidth : g.finf->defaultWidth.glyphWidth;
    int left = cwi ? cwi->left : g.finf->defaultWidth.left;
    float advance = (float)(cwi ? cwi->charWidth : g.finf->defaultWidth.charWidth) * gs;
    if (!plot || cp == ' ' || glyph_w <= 0 || gs <= 0.0f) {
        return advance;
    }

    const TGLP_s *t = g.finf->tglp;
    fontGlyphPos_s pos;
    C2D_FontCalcGlyphPos(g.font, &pos, g.glyph, 0, 1.0f, 1.0f);
    if (pos.sheetIndex < 0 || pos.sheetIndex >= t->nSheets) {
        return advance;
    }
    const u8 *sheet = t->sheetData + (size_t)pos.sheetIndex * t->sheetSize;
    int sx0 = (int)(pos.texcoord.left * (float)t->sheetWidth + 0.5f);
    int sy0 = (int)((1.0f - pos.texcoord.top) * (float)t->sheetHeight + 0.5f);
    int glyph_h = t->cellHeight;

    const TGLP_s *sys = C2D_FontGetInfo(NULL)->tglp;
    float ox = x + (float)left * gs;
    float oy = (float)y + (float)sys->baselinePos * scale - (float)t->baselinePos * gs;
    int dx0 = (int)floorf(ox);
    int dy0 = (int)floorf(oy);
    int dw = (int)ceilf(ox + (float)glyph_w * gs) - dx0;
    int dh = (int)ceilf(oy + (float)glyph_h * gs) - dy0;
    int n = gs >= 0.75f ? 1 : (gs >= 0.4f ? 2 : 3);
    float inv = 1.0f / gs;
    float step = 1.0f / (float)n;
    int div = n * n;

    for (int dy = 0; dy < dh; dy++) {
        for (int dx = 0; dx < dw; dx++) {
            int sum = 0;
            for (int j = 0; j < n; j++) {
                float fy = ((float)(dy0 + dy) + ((float)j + 0.5f) * step - oy) * inv;
                if (fy < 0.0f || fy >= (float)glyph_h) {
                    continue;
                }
                for (int i = 0; i < n; i++) {
                    float fx = ((float)(dx0 + dx) + ((float)i + 0.5f) * step - ox) * inv;
                    if (fx < 0.0f || fx >= (float)glyph_w) {
                        continue;
                    }
                    sum += sysfont_sheet_alpha(t, sheet, sx0 + (int)fx, sy0 + (int)fy);
                }
            }
            int a = sum / div;
            if (a > 0) {
                plot(ctx, dx0 + dx, dy0 + dy, a);
            }
        }
    }
    return advance;
}

static float sysfont_text_width(const char *text, float scale)
{
    float w = 0.0f;
    const char *p = text ? text : "";
    const char *end = p + strlen(p);
    while (p < end && *p != '\n') {
        u32 cp = 0;
        size_t len = read_utf8_codepoint(p, end, &cp);
        if (!len) {
            len = 1;
            cp = '?';
        }
        w += sysfont_advance(cp, scale);
        p += len;
    }
    return w;
}

/* Draws one line (stops at '\n'); with max_w > 0 a line that doesn't fit is
 * cut short with "...". Returns the drawn width. */
static float sysfont_draw_text(const char *text, float x, int y, float scale, float max_w, SysPlotFn plot, void *ctx)
{
    const char *p = text ? text : "";
    const char *end = p + strlen(p);
    bool clip = max_w > 0.0f && sysfont_text_width(p, scale) > max_w;
    float ellipsis = clip ? 3.0f * sysfont_advance('.', scale) : 0.0f;
    float pen = x;
    while (p < end && *p != '\n') {
        u32 cp = 0;
        size_t len = read_utf8_codepoint(p, end, &cp);
        if (!len) {
            len = 1;
            cp = '?';
        }
        if (clip && pen - x + sysfont_advance(cp, scale) > max_w - ellipsis) {
            break;
        }
        pen += sysfont_draw_glyph(cp, pen, y, scale, plot, ctx);
        p += len;
    }
    if (clip) {
        for (int i = 0; i < 3; i++) {
            pen += sysfont_draw_glyph('.', pen, y, scale, plot, ctx);
        }
    }
    return pen - x;
}

/* Greedy word wrap into at most max_lines lines; long runs without spaces
 * (Japanese, Chinese) break between characters. */
#define WRAP_LINE_CAP 320

static int sysfont_wrap(const char *text, float scale, float max_w, char lines[][WRAP_LINE_CAP], int max_lines)
{
    int count = 0;
    const char *p = text ? text : "";
    while (*p && count < max_lines) {
        const char *pe = strchr(p, '\n');
        if (!pe) {
            pe = p + strlen(p);
        }
        const char *ls = p;
        while (ls < pe && count < max_lines) {
            while (ls < pe && *ls == ' ') {
                ls++;
            }
            if (ls >= pe) {
                break;
            }
            float w = 0.0f;
            const char *q = ls;
            const char *last_space = NULL;
            const char *brk = pe;
            while (q < pe) {
                u32 cp = 0;
                size_t len = read_utf8_codepoint(q, pe, &cp);
                if (!len) {
                    len = 1;
                    cp = '?';
                }
                float adv = sysfont_advance(cp, scale);
                if (w + adv > max_w && q > ls) {
                    brk = last_space && last_space > ls ? last_space : q;
                    break;
                }
                if (cp == ' ') {
                    last_space = q;
                }
                w += adv;
                q += len;
            }
            if (q >= pe) {
                brk = pe;
            }
            size_t n = (size_t)(brk - ls);
            while (n > 0 && ls[n - 1] == ' ') {
                n--;
            }
            if (n >= WRAP_LINE_CAP) {
                n = WRAP_LINE_CAP - 1;
                while (n > 0 && ((u8)ls[n] & 0xC0) == 0x80) {
                    n--; /* don't cut a UTF-8 sequence in half */
                }
            }
            memcpy(lines[count], ls, n);
            lines[count][n] = 0;
            count++;
            ls = brk;
        }
        p = *pe ? pe + 1 : pe;
    }
    return count;
}

/* ---- Subtitle overlay (top screen, RGB565, rotated) ------------------- */

#define SUB_MASK_W 400
#define SUB_MASK_H 120
#define SUB_MAX_LINES 4
#define SUB_TEXT_PX 18.0f
#define SUB_BOTTOM_MARGIN 8

typedef struct {
    u8 *mask; /* column-major: mask[x * SUB_MASK_H + y] */
    int h;
} SubMaskCtx;

static u8 *g_sub_mask;
static u8 *g_sub_edge;
static int g_sub_mask_top;   /* screen y of mask row 0 */
static int g_sub_mask_rows;
static int g_sub_box_x0, g_sub_box_x1, g_sub_box_y0, g_sub_box_y1;
static bool g_sub_has_pixels;
static int g_sub_cache_cues[4];
static int g_sub_cache_count = -1;
static u32 g_sub_cache_generation;

static void sub_mask_plot(void *vctx, int x, int y, int alpha)
{
    SubMaskCtx *ctx = (SubMaskCtx *)vctx;
    if (x < 0 || x >= SUB_MASK_W || y < 0 || y >= ctx->h) {
        return;
    }
    u8 *m = &ctx->mask[x * SUB_MASK_H + y];
    if (alpha > *m) {
        *m = (u8)alpha;
    }
}

static void subtitle_overlay_reset(void)
{
    g_sub_cache_count = -1;
    g_sub_has_pixels = false;
}

static void subtitle_overlay_free(void)
{
    free(g_sub_mask);
    free(g_sub_edge);
    g_sub_mask = NULL;
    g_sub_edge = NULL;
    subtitle_overlay_reset();
}

static void subtitle_overlay_build(const int *cues, int count)
{
    g_sub_has_pixels = false;
    if (count <= 0) {
        return;
    }
    if (!g_sub_mask) {
        g_sub_mask = (u8 *)malloc(SUB_MASK_W * SUB_MASK_H);
        g_sub_edge = (u8 *)malloc(SUB_MASK_W * SUB_MASK_H);
        if (!g_sub_mask || !g_sub_edge) {
            subtitle_overlay_free();
            return;
        }
    }

    char text[1024];
    size_t used = 0;
    text[0] = 0;
    for (int i = 0; i < count; i++) {
        const char *t = g_subs.text + g_subs.cues[cues[i]].text_off;
        int n = snprintf(text + used, sizeof(text) - used, "%s%s", used ? "\n" : "", t);
        if (n < 0 || (size_t)n >= sizeof(text) - used) {
            used = strlen(text);
            break;
        }
        used += (size_t)n;
    }

    float scale = sysfont_scale_for_px(SUB_TEXT_PX);
    static char lines[SUB_MAX_LINES][WRAP_LINE_CAP];
    int nlines = sysfont_wrap(text, scale, 384.0f, lines, SUB_MAX_LINES);
    if (nlines <= 0) {
        return;
    }

    int line_h = (int)ceilf(SUB_TEXT_PX);
    int rows = nlines * line_h + 4;
    if (rows > SUB_MASK_H) {
        rows = SUB_MASK_H;
    }
    for (int x = 0; x < SUB_MASK_W; x++) {
        memset(g_sub_mask + x * SUB_MASK_H, 0, (size_t)rows);
        memset(g_sub_edge + x * SUB_MASK_H, 0, (size_t)rows);
    }

    SubMaskCtx ctx = { g_sub_mask, rows };
    for (int i = 0; i < nlines; i++) {
        float w = sysfont_text_width(lines[i], scale);
        float x = ((float)SUB_MASK_W - w) * 0.5f;
        sysfont_draw_text(lines[i], x, 2 + i * line_h, scale, 0.0f, sub_mask_plot, &ctx);
    }

    /* A 1 px dark outline (3x3 dilation) keeps white text readable on any video. */
    int x0 = SUB_MASK_W, x1 = -1, y0 = rows, y1 = -1;
    for (int x = 0; x < SUB_MASK_W; x++) {
        u8 *e = g_sub_edge + x * SUB_MASK_H;
        for (int y = 0; y < rows; y++) {
            int best = 0;
            for (int ax = x - 1; ax <= x + 1; ax++) {
                if (ax < 0 || ax >= SUB_MASK_W) {
                    continue;
                }
                const u8 *col = g_sub_mask + ax * SUB_MASK_H;
                for (int ay = y - 1; ay <= y + 1; ay++) {
                    if (ay >= 0 && ay < rows && col[ay] > best) {
                        best = col[ay];
                    }
                }
            }
            e[y] = (u8)best;
            if (best) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < 0) {
        return;
    }

    g_sub_mask_rows = rows;
    g_sub_mask_top = 240 - SUB_BOTTOM_MARGIN - rows;
    if (g_sub_mask_top < 0) {
        g_sub_mask_top = 0;
    }
    g_sub_box_x0 = x0;
    g_sub_box_x1 = x1 + 1;
    g_sub_box_y0 = y0;
    g_sub_box_y1 = y1 + 1;
    g_sub_has_pixels = true;
}

static inline u16 sub_blend565(u16 dst, u16 src, int a)
{
    int ia = 255 - a;
    int r = (((src >> 11) & 0x1F) * a + ((dst >> 11) & 0x1F) * ia) / 255;
    int g = (((src >> 5) & 0x3F) * a + ((dst >> 5) & 0x3F) * ia) / 255;
    int b = ((src & 0x1F) * a + (dst & 0x1F) * ia) / 255;
    return (u16)((r << 11) | (g << 5) | b);
}

/* Fallback when the system font can't be read: the built-in pixel font. */
static void subtitle_overlay_draw_pixel_font(u16 *fb, const int *cues, int count)
{
    int y = 240 - SUB_BOTTOM_MARGIN - count * 10;
    for (int i = 0; i < count; i++) {
        char line[96];
        copy_safe(line, sizeof(line), g_subs.text + g_subs.cues[cues[i]].text_off);
        for (char *c = line; *c; c++) {
            if (*c == '\n') {
                *c = ' ';
            }
        }
        int w = bottom_text_width(line, 1);
        fb_draw_text_stroked(fb, (400 - w) / 2, y + i * 10, line, 1, 0xFFFF, 255);
    }
}

/* Draws the subtitle for media time `ticks` onto a top-screen frame. */
static void subtitle_overlay_draw(u16 *fb, u64 ticks)
{
    if (!fb || !g_subs.cues || strcmp(g_subs.item_id, g_current.id) != 0 ||
        g_subs.stream_index != tracks_selected_text_sub_index()) {
        return;
    }

    int cues[4];
    int count = subtitles_active_cues((u32)(ticks / 10000ULL), cues, 4);
    if (!sysfont_ready()) {
        subtitle_overlay_draw_pixel_font(fb, cues, count);
        return;
    }

    bool same = count == g_sub_cache_count && g_sub_cache_generation == g_sub_generation;
    for (int i = 0; same && i < count; i++) {
        same = cues[i] == g_sub_cache_cues[i];
    }
    if (!same) {
        subtitle_overlay_build(cues, count);
        g_sub_cache_count = count;
        g_sub_cache_generation = g_sub_generation;
        memcpy(g_sub_cache_cues, cues, sizeof(int) * (size_t)count);
    }
    if (!g_sub_has_pixels) {
        return;
    }

    const u16 white = 0xFFFF;
    const u16 black = 0x0000;
    for (int x = g_sub_box_x0; x < g_sub_box_x1; x++) {
        u16 *col = fb + (size_t)x * 240;
        const u8 *mc = g_sub_mask + x * SUB_MASK_H;
        const u8 *ec = g_sub_edge + x * SUB_MASK_H;
        for (int my = g_sub_box_y0; my < g_sub_box_y1; my++) {
            int e = ec[my];
            if (!e) {
                continue;
            }
            int sy = g_sub_mask_top + my;
            if (sy < 0 || sy >= 240) {
                continue;
            }
            u16 *px = &col[239 - sy];
            u16 v = sub_blend565(*px, black, (e * 230) / 255);
            int t = mc[my];
            if (t) {
                v = sub_blend565(v, white, t);
            }
            *px = v;
        }
    }
}

/* ---- Bottom screen text (BGR8, rotated) -------------------------------- */

typedef struct {
    u8 *fb;
    u8 r, g, b;
} BottomPlotCtx;

static void bottom_plot(void *vctx, int x, int y, int a)
{
    BottomPlotCtx *ctx = (BottomPlotCtx *)vctx;
    if (x < 0 || x >= 320 || y < 0 || y >= 240) {
        return;
    }
    u8 *p = ctx->fb + ((size_t)x * 240 + (size_t)(239 - y)) * 3;
    int ia = 255 - a;
    p[0] = (u8)((ctx->b * a + p[0] * ia) / 255);
    p[1] = (u8)((ctx->g * a + p[1] * ia) / 255);
    p[2] = (u8)((ctx->r * a + p[2] * ia) / 255);
}

/* Draws text px pixels tall with its top at y; cut short with "..." past max_w. */
static int bottom_sys_text(u8 *fb, int x, int y, const char *text, float px, int max_w, u8 r, u8 g, u8 b)
{
    if (!text || !text[0]) {
        return 0;
    }
    if (!sysfont_ready()) {
        bottom_draw_text(fb, x, y + ((int)px - 7) / 2, text, 1, r, g, b);
        return bottom_text_width(text, 1);
    }
    BottomPlotCtx ctx = { fb, r, g, b };
    return (int)ceilf(sysfont_draw_text(text, (float)x, y, sysfont_scale_for_px(px), (float)max_w, bottom_plot, &ctx));
}

static int bottom_sys_text_width(const char *text, float px)
{
    if (!sysfont_ready()) {
        return bottom_text_width(text, 1);
    }
    return (int)ceilf(sysfont_text_width(text, sysfont_scale_for_px(px)));
}

/* Wrapped, centred message inside a column; returns lines used. */
static int bottom_sys_message(u8 *fb, int x, int y, int w, const char *text, float px, u8 r, u8 g, u8 b)
{
    static char lines[3][WRAP_LINE_CAP];
    int n;
    if (sysfont_ready()) {
        n = sysfont_wrap(text, sysfont_scale_for_px(px), (float)w, lines, 3);
    } else {
        copy_safe(lines[0], sizeof(lines[0]), text);
        n = 1;
    }
    for (int i = 0; i < n; i++) {
        int lw = bottom_sys_text_width(lines[i], px);
        bottom_sys_text(fb, x + (w - lw) / 2, y + i * ((int)px + 2), lines[i], px, w, r, g, b);
    }
    return n;
}

/* ---- Picker ------------------------------------------------------------- */

#define TRACK_PANEL_Y 8
#define TRACK_PANEL_H 196
#define TRACK_PANEL_W 148
#define TRACK_ROWS_Y 36
#define TRACK_ROW_H 32
#define TRACK_VISIBLE_ROWS 5

static void track_menu_draw_column(u8 *fb, int column)
{
    int px = 8 + column * 156;
    bool active = g_track_menu.column == column;
    bottom_fill_rect(fb, px, TRACK_PANEL_Y, TRACK_PANEL_W, TRACK_PANEL_H, 21, 23, 29);
    if (active) {
        bottom_stroke_rect(fb, px, TRACK_PANEL_Y, TRACK_PANEL_W, TRACK_PANEL_H, 55, 206, 224);
        bottom_fill_rect(fb, px, TRACK_PANEL_Y, TRACK_PANEL_W, 2, 55, 206, 224);
    } else {
        bottom_stroke_rect(fb, px, TRACK_PANEL_Y, TRACK_PANEL_W, TRACK_PANEL_H, 42, 47, 59);
    }
    const char *heading = column == 0 ? tr(TR_TRACKS_AUDIO) : tr(TR_TRACKS_SUBTITLES);
    u8 hc = active ? 245 : 150;
    bottom_sys_text(fb, px + 10, TRACK_PANEL_Y + 7, heading, 15.0f, TRACK_PANEL_W - 20, hc, active ? 247 : 158, active ? 250 : 172);

    int rows = track_menu_rows(column);
    int *cursor = &g_track_menu.cursor[column];
    int *scroll = &g_track_menu.scroll[column];
    if (*cursor >= rows) {
        *cursor = rows > 0 ? rows - 1 : 0;
    }
    if (*cursor < *scroll) {
        *scroll = *cursor;
    } else if (*cursor >= *scroll + TRACK_VISIBLE_ROWS) {
        *scroll = *cursor - TRACK_VISIBLE_ROWS + 1;
    }

    int selected_row = -1;
    if (column == 0) {
        int audio = tracks_effective_audio_index();
        for (int i = 0; i < g_audio_track_count; i++) {
            if (g_audio_tracks[i].index == audio) {
                selected_row = i;
            }
        }
    } else {
        selected_row = 0;
        for (int i = 0; i < g_sub_track_count; i++) {
            if (g_sub_tracks[i].index == g_sel_sub_index) {
                selected_row = i + 1;
            }
        }
    }

    int shown = 0;
    for (int row = *scroll; row < rows && shown < TRACK_VISIBLE_ROWS; row++, shown++) {
        int ry = TRACK_ROWS_Y + shown * TRACK_ROW_H;
        bool focused = active && row == *cursor;
        bool chosen = row == selected_row;
        if (focused) {
            bottom_fill_rect(fb, px + 4, ry, TRACK_PANEL_W - 8, TRACK_ROW_H - 2, 31, 52, 60);
            bottom_stroke_rect(fb, px + 4, ry, TRACK_PANEL_W - 8, TRACK_ROW_H - 2, 55, 206, 224);
        }
        if (chosen) {
            bottom_fill_rect(fb, px + 10, ry + 11, 6, 6, 55, 206, 224);
        }

        const char *title;
        const char *detail = "";
        if (column == 0) {
            title = g_audio_tracks[row].title;
            detail = g_audio_tracks[row].detail;
        } else if (row == 0) {
            title = tr(TR_TRACKS_OFF);
        } else {
            title = g_sub_tracks[row - 1].title;
            detail = g_sub_tracks[row - 1].detail;
        }
        int tx = px + 22;
        int tw = TRACK_PANEL_W - 22 - 10;
        u8 tc = chosen || focused ? 245 : 214;
        if (detail[0]) {
            bottom_sys_text(fb, tx, ry + 3, title, 14.0f, tw, tc, tc, tc);
            bottom_sys_text(fb, tx, ry + 17, detail, 11.0f, tw, 150, 158, 172);
        } else {
            bottom_sys_text(fb, tx, ry + 8, title, 14.0f, tw, tc, tc, tc);
        }
    }

    if (rows > TRACK_VISIBLE_ROWS) {
        int track_h = TRACK_VISIBLE_ROWS * TRACK_ROW_H - 2;
        int thumb_h = track_h * TRACK_VISIBLE_ROWS / rows;
        int thumb_y = TRACK_ROWS_Y + (track_h - thumb_h) * *scroll / (rows - TRACK_VISIBLE_ROWS);
        bottom_fill_rect(fb, px + TRACK_PANEL_W - 4, TRACK_ROWS_Y, 2, track_h, 42, 47, 59);
        bottom_fill_rect(fb, px + TRACK_PANEL_W - 4, thumb_y, 2, thumb_h, 55, 206, 224);
    }

    const char *message = NULL;
    if (column == 0) {
        message = g_audio_track_count == 0 ? tr(TR_TRACKS_NO_AUDIO) :
                  g_audio_track_count == 1 ? tr(TR_TRACKS_NO_OTHER_AUDIO) : NULL;
    } else if (g_sub_track_count == 0) {
        message = tr(TR_TRACKS_NO_SUBTITLES);
    }
    if (message && shown < TRACK_VISIBLE_ROWS) {
        int my = TRACK_ROWS_Y + shown * TRACK_ROW_H + 8;
        bottom_sys_message(fb, px + 8, my, TRACK_PANEL_W - 16, message, 12.0f, 150, 158, 172);
    }
}

static void track_menu_draw_bottom(u8 *fb)
{
    bottom_fill_rect(fb, 0, 0, 320, 240, 14, 15, 19);
    track_menu_draw_column(fb, 0);
    track_menu_draw_column(fb, 1);

    const char *footer = tr(TR_TRACKS_HINT);
    u8 r = 150, g = 158, b = 172;
    SubtitleState state = subtitles_state();
    if (state == SUB_STATE_LOADING) {
        footer = tr(TR_TRACKS_LOADING);
        r = 55; g = 206; b = 224;
    } else if (state == SUB_STATE_FAILED) {
        footer = tr(TR_TRACKS_LOAD_FAILED);
        r = 240; g = 128; b = 128;
    }
    int fw = bottom_sys_text_width(footer, 13.0f);
    if (fw > 304) {
        fw = 304;
    }
    bottom_sys_text(fb, (320 - fw) / 2, 215, footer, 13.0f, 304, r, g, b);
}
