/* Music has square artwork and song rows; video shelves keep their own layout. */
static bool music_browse_view(void)
{
    return g_view == VIEW_ITEMS && (music_is_root_type(g_current_parent_type) ||
        !strcmp(g_current_parent_type, "MusicAlbum") || !strcmp(g_current_parent_type, "MusicArtist"));
}

static bool music_track_list(void)
{
    return music_browse_view() && (!strcmp(g_current_parent_type, "MusicSongs") || !strcmp(g_current_parent_type, "MusicAlbum"));
}

static const char *music_mode_name(int mode)
{
    return mode == 1 ? "Artists" : mode == 2 ? "Songs" : "Albums";
}

static void music_activate_mode(void)
{
    char parent[80], title[96];
    copy_safe(parent, sizeof(parent), g_current_parent_id);
    copy_safe(title, sizeof(title), g_screen_title);
    if (load_items_for_parent_ex(parent, title, music_mode_type(g_music_nav_mode), "")) {
        g_music_mode_focus = false;
        g_browse_scroll.ready = false;
    } else g_browse_notice_until = osGetTime() + 4000;
}

static void draw_music_browse(void)
{
    if (music_is_root_type(g_current_parent_type)) {
        int active = music_mode_for_type(g_current_parent_type);
        for (int mode = 0; mode < 3; ++mode) {
            float x = 10 + mode * 130;
            bool focus = g_music_mode_focus && mode == g_music_nav_mode;
            C2D_DrawRectSolid(x, 38, 0, 120, 23, focus ? 0xFF40302B : 0xFF252121);
            draw_text_centered(x + 60, 41, 0.5f, focus || active == mode ? COL_PRIMARY : COL_MUTED, "%s", music_mode_name(mode));
            if (active == mode) C2D_DrawRectSolid(x + 24, 59, 0, 72, 2, COL_PRIMARY);
        }
    } else {
        browse_label(10, 40, 308, 0.55f, COL_WHITE, g_screen_title);
        draw_text(325, 41, 0.45f, COL_MUTED, "%d/%d", g_item_count ? g_selected + 1 : 0, g_item_count);
    }
    if (!g_item_count) draw_text(14, 100, 0.55f, COL_MUTED, "No music here yet");
    if (music_track_list()) {
        int first = (int)(g_browse_scroll.value / 48);
        for (int i = first; i < g_item_count && i < first + 5; ++i) {
            MediaItem *item = &g_items[i];
            float y = GRID_CARD_TOP + i * 48 - g_browse_scroll.value;
            if (y >= BROWSE_CONTENT_BOTTOM) break;
            bool focused = !g_tab_focus && !g_music_mode_focus && i == g_selected;
            browse_rect(8, y, 384, 46, focused ? 0xFF302B23 : 0xFF201D1D, true);
            if (focused) browse_rect(8, y, 3, 46, COL_PRIMARY, true);
            if (y + 13 >= browse_clip_top() && y + 29 <= BROWSE_CONTENT_BOTTOM) {
                draw_text(17, y + 13, 0.5f, focused ? COL_PRIMARY : COL_MUTED, "%02d", item->index_number > 0 ? item->index_number : i + 1);
                char duration[32] = "--:--";
                if (item->runtime_ticks) format_playback_time(item->runtime_ticks, duration, sizeof(duration));
                browse_label(334, y + 13, 49, 0.5f, COL_MUTED, duration);
            }
            if (y + 3 >= browse_clip_top() && y + 18 <= BROWSE_CONTENT_BOTTOM)
                browse_label(54, y + 3, 270, 0.5f, COL_WHITE, item->name);
            if (y + 25 >= browse_clip_top() && y + 40 <= BROWSE_CONTENT_BOTTOM) {
                char detail[256];
                if (item->parent_index_number > 1) snprintf(detail, sizeof(detail), "Disc %d  /  %s", item->parent_index_number, item->artist[0] ? item->artist : "Unknown artist");
                else copy_safe(detail, sizeof(detail), item->artist[0] ? item->artist : "Unknown artist");
                browse_label(54, y + 25, 270, 0.5f, COL_MUTED, detail);
                if (item->is_favorite) draw_text(364, y + 29, 0.5f, COL_SECONDARY, "\xE2\x99\xA5");
            }
        }
    } else {
        int first = (int)(g_browse_scroll.value / 150);
        for (int i = first * 3; i < g_item_count && i < (first + 3) * 3; ++i) {
            float x = 12 + (i % 3) * 130, y = GRID_CARD_TOP + (i / 3) * 150 - g_browse_scroll.value;
            if (y >= BROWSE_CONTENT_BOTTOM) break;
            MediaItem *item = &g_items[i];
            if (!g_tab_focus && !g_music_mode_focus && i == g_selected) browse_rect(x - 2, y - 2, 114, 114, COL_PRIMARY, true);
            browse_art(item, BROWSE_ART_SQUARE, x, y, 110, 110, true);
            if (y + 113 >= browse_clip_top() && y + 128 <= BROWSE_CONTENT_BOTTOM)
                browse_label(x, y + 113, 116, 0.5f, COL_WHITE, item->name);
            if (y + 130 >= browse_clip_top() && y + 145 <= BROWSE_CONTENT_BOTTOM)
                browse_label(x, y + 130, 116, 0.5f, COL_MUTED, item->artist[0] ? item->artist : !strcmp(item->type, "MusicArtist") ? "Artist" : "Album");
        }
    }
}

static void draw_music_preview(const MediaItem *item)
{
    browse_art(item, BROWSE_ART_SQUARE, 12, 12, 124, 124, false);
    browse_label(150, 12, 158, 0.5f, COL_PRIMARY, music_is_audio(item) ? "SONG" : !strcmp(item->type, "MusicArtist") ? "ARTIST" : "ALBUM");
    draw_text_lines(150, 36, 0.6f, 158, COL_WHITE, item->name, 3, 20);
    draw_text_lines(150, 102, 0.5f, 158, COL_MUTED, item->artist, 2, 17);
    char detail[256];
    if (music_is_audio(item)) {
        char duration[32] = "";
        if (item->runtime_ticks) format_playback_time(item->runtime_ticks, duration, sizeof(duration));
        snprintf(detail, sizeof(detail), "%s%s%s", item->album[0] ? item->album : "Song", duration[0] ? "  /  " : "", duration);
    } else if (item->year > 0) snprintf(detail, sizeof(detail), "%d%s", item->year, item->child_count > 0 ? "  /  Open to browse songs" : "");
    else copy_safe(detail, sizeof(detail), !strcmp(item->type, "MusicArtist") ? "Explore albums by this artist" : "Open to browse songs");
    draw_text_lines(12, 143, 0.5f, 296, COL_MUTED, detail, 2, 17);
}

static const char *music_status_label(void)
{
    return g_music.phase == MUSIC_ERROR ? "Couldn't play" : g_music.phase == MUSIC_ENDED ? "Queue finished" :
        g_music.paused ? "Paused" : g_music.buffering ? "Buffering..." : "Now playing";
}

static void draw_music_top(void)
{
    C2D_DrawRectSolid(0, 0, 0, 400, 240, COL_BG);
    C2D_DrawRectSolid(0, 0, 0, 400, 28, 0xFF241B22);
    draw_text(12, 7, 0.5f, COL_SECONDARY, "MUSIC");
    int rate = g_music.audio_rate ? (int)g_music.audio_rate : g_music.sound.sample_rate;
    int channels = g_music.audio_channels ? g_music.audio_channels : g_music.sound.channels;
    draw_text(88, 7, 0.45f, COL_MUTED, "%.3g kHz / %s", rate / 1000.0, channels == 1 ? "Mono" : "Stereo");
    draw_text(285, 7, 0.5f, COL_MUTED, "%d / %d", g_music.index + 1, g_music.count);
    C2D_DrawRectSolid(10, 34, 0, 188, 188, 0xFF47333D);
    browse_art(&g_current, BROWSE_ART_SQUARE, 12, 36, 184, 184, false);
    browse_label(210, 35, 178, 0.5f, COL_PRIMARY, music_status_label());
    draw_text_lines(210, 61, 0.6f, 178, COL_WHITE, g_current.name, 3, 20);
    if (g_music.phase == MUSIC_ERROR) {
        draw_text_lines(210, 131, 0.5f, 178, COL_MUTED, g_music.error, 5, 18);
    } else {
        draw_text_lines(210, 131, 0.55f, 178, COL_WHITE, g_current.artist[0] ? g_current.artist : "Unknown artist", 2, 18);
        draw_text_lines(210, 177, 0.5f, 178, COL_MUTED, g_current.album, 2, 17);
        if (g_music.index + 1 < g_music.count && g_music.queue[g_music.index + 1] >= 0) {
            char next[160]; snprintf(next, sizeof(next), "Next: %s", g_items[g_music.queue[g_music.index + 1]].name);
            browse_label(12, 224, 376, 0.45f, COL_MUTED, next);
        }
    }
}

static void draw_music_bottom(void)
{
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COL_BG);
    draw_text_lines(20, 10, 0.55f, 280, COL_WHITE, g_current.name, 2, 19);
    browse_label(20, 54, 280, 0.5f, COL_PRIMARY, music_status_label());
    char elapsed[32], duration[32] = "--:--";
    u64 position = music_position();
    format_playback_time(position, elapsed, sizeof(elapsed));
    if (g_current.runtime_ticks) format_playback_time(g_current.runtime_ticks, duration, sizeof(duration));
    draw_text(20, 77, 0.5f, COL_MUTED, "%s", elapsed);
    browse_label(249, 77, 51, 0.5f, COL_MUTED, duration);
    C2D_DrawRectSolid(20, 102, 0, 280, 4, 0xFF403438);
    float ratio = g_current.runtime_ticks ? (double)position / g_current.runtime_ticks : 0;
    if (ratio > 1) ratio = 1;
    C2D_DrawRectSolid(20, 102, 0, 280 * ratio, 4, COL_PRIMARY);
    C2D_DrawCircleSolid(20 + 280 * ratio, 104, 0, 5, COL_PRIMARY);
    draw_text_centered(160, 112, 0.45f, COL_MUTED, "Vol %d%%   SELECT: Sound", g_music.muted ? 0 : g_cfg.music.volume_percent);
    C2D_DrawRectSolid(10, 132, 0, 82, 44, COL_PAPER);
    C2D_DrawRectSolid(100, 132, 0, 120, 44, COL_PRIMARY_DARK);
    C2D_DrawRectSolid(228, 132, 0, 82, 44, COL_PAPER);
    draw_text_centered(51, 145, 0.5f, COL_WHITE, "Previous");
    draw_text_centered(160, 144, 0.6f, COL_WHITE, "%s", g_music.phase == MUSIC_ERROR ? "Retry" : g_music.phase == MUSIC_ENDED ? "Replay" : g_music.paused ? "Play" : "Pause");
    draw_text_centered(269, 145, 0.5f, COL_WHITE, "Next");
    C2D_DrawRectSolid(10, 182, 0, 98, 27, 0xFF30232D);
    C2D_DrawRectSolid(113, 182, 0, 94, 27, COL_PAPER);
    C2D_DrawRectSolid(212, 182, 0, 98, 27, COL_PAPER);
    draw_text_centered(59, 187, 0.45f, g_current.is_favorite ? COL_SECONDARY : COL_WHITE, "%s", g_current.is_favorite ? "Unfavorite" : "Favorite");
    draw_text_centered(160, 187, 0.5f, COL_WHITE, "%s", g_music.muted ? "Unmute" : "Mute");
    draw_text_centered(261, 187, 0.5f, COL_PRIMARY, "Sound");
    C2D_DrawRectSolid(10, 213, 0, 300, 26, COL_PAPER);
    draw_text_centered(160, 217, 0.5f, COL_WHITE, "B: Stop and go back");
}

static const char *music_setting_label(int row)
{
    static const char *labels[] = {"Sample rate", "Channels", "Buffer", "Volume", "Restore defaults", "Save and return"};
    return labels[clamp_int(row, 0, 5)];
}

static void draw_music_settings_top(void)
{
    const MusicSoundSettings *sound = &g_music_settings_draft;
    C2D_DrawRectSolid(0, 0, 0, 400, 240, COL_BG);
    C2D_DrawRectSolid(0, 0, 0, 400, 32, 0xFF241B22);
    draw_text(16, 7, 0.6f, COL_WHITE, "Music sound");
    C2D_DrawRectSolid(18, 44, 0, 364, 83, COL_PAPER);
    C2D_DrawRectSolid(18, 44, 0, 3, 83, COL_PRIMARY);
    const char *rate = sound->sample_rate == 44100 ? "44.1 kHz" : sound->sample_rate == 32000 ? "32 kHz" : "48 kHz";
    draw_text(32, 49, 0.85f, COL_WHITE, "%s", rate);
    draw_text(212, 57, 0.5f, COL_PRIMARY, "%s", sound->sample_rate == 48000 ? "Very high" : sound->sample_rate == 44100 ? "High" : "Data saver");
    draw_text(32, 84, 0.5f, COL_WHITE, "16-bit PCM / %s", sound->channels == 1 ? "Mono" : "Stereo");
    double kbps = sound->sample_rate * sound->channels * 16 / 1000.0;
    draw_text(32, 107, 0.45f, COL_MUTED, "%.1f kbps / %.1f MB per min", kbps, kbps * 60 / 8000);
    draw_text(18, 141, 0.55f, COL_PRIMARY, "%s", music_setting_label(g_music_settings_row));
    static const char *notes[] = {
        "48 kHz is recommended for music. Lower rates use less bandwidth. The 3DS resamples for its output.",
        "Stereo preserves left and right detail. Mono combines both channels and uses half the bandwidth.",
        "1,024 KB is recommended. Going below this is not recommended, especially on Old 3DS.",
        "Music volume is separate from video. 100% keeps the original level without extra amplification.",
        "Restores 48 kHz, stereo, a 1,024 KB buffer and 100% volume. Choose Save to apply.",
        "Saves for future songs. Quality changes briefly reload the current song at the same position."
    };
    draw_text_lines(18, 166, 0.5f, 364, COL_MUTED, notes[clamp_int(g_music_settings_row, 0, 5)], 4, 17);
}

static void draw_music_settings_bottom(void)
{
    const MusicSoundSettings *sound = &g_music_settings_draft;
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COL_BG);
    draw_text_centered(160, 8, 0.5f, COL_MUTED, "Left / Right: Adjust");
    for (int row = 0; row < 4; ++row) {
        float y = 34 + row * 32;
        bool selected = row == g_music_settings_row;
        C2D_DrawRectSolid(10, y, 0, 300, 28, selected ? COL_PRIMARY_DARK : COL_PAPER);
        if (selected) C2D_DrawRectSolid(10, y, 0, 3, 28, COL_PRIMARY);
        draw_text(22, y + 5, 0.5f, COL_WHITE, "%s", music_setting_label(row));
        char value[48];
        if (row == 0) snprintf(value, sizeof(value), "%s", sound->sample_rate == 44100 ? "44.1 kHz" : sound->sample_rate == 32000 ? "32 kHz" : "48 kHz");
        else if (row == 1) snprintf(value, sizeof(value), "%s", sound->channels == 1 ? "Mono" : "Stereo");
        else if (row == 2) snprintf(value, sizeof(value), "%d KB", sound->buffer_kb);
        else snprintf(value, sizeof(value), "%d%%", sound->volume_percent);
        draw_text_centered(225, y + 5, 0.5f, selected ? COL_WHITE : COL_PRIMARY, "%s", value);
        draw_text(292, y + 5, 0.5f, COL_MUTED, ">");
    }
    C2D_DrawRectSolid(10, 174, 0, 145, 33, g_music_settings_row == 4 ? COL_PRIMARY_DARK : COL_PAPER);
    C2D_DrawRectSolid(165, 174, 0, 145, 33, g_music_settings_row == 5 ? COL_PRIMARY_DARK : 0xFF40302B);
    draw_text_centered(82, 182, 0.5f, COL_WHITE, "Defaults");
    draw_text_centered(237, 182, 0.5f, COL_WHITE, "Save");
    draw_text_centered(160, 220, 0.5f, COL_MUTED, "B: Cancel   SELECT: Save");
}
