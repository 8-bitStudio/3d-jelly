static void app_apt_hook(APT_HookType hook, void *param)
{
    (void)param;
    if (hook == APTHOOK_ONEXIT) {
        g_exit_requested = true;
        g_system_close_requested = true;
    }
}

static bool app_system_closing(void)
{
    return g_system_close_requested || aptShouldClose();
}

static bool app_should_exit(void)
{
    if (g_exit_requested || aptShouldClose()) {
        g_exit_requested = true;
        if (aptShouldClose()) {
            g_system_close_requested = true;
        }
        return true;
    }
    return false;
}

static bool app_keep_running(void)
{
    if (!aptMainLoop()) {
        g_exit_requested = true;
        g_system_close_requested = true;
        return false;
    }
    return !app_should_exit();
}

static bool app_wait_or_exit(u64 ns)
{
    while (ns > 0) {
        if (!app_keep_running()) {
            return true;
        }
        u64 step = ns < EXIT_POLL_SLEEP_NS ? ns : EXIT_POLL_SLEEP_NS;
        svcSleepThread(step);
        ns -= step;
    }
    return app_should_exit();
}

static void playback_graphics_exit(void)
{
    if (!app_system_closing() && !g_playback_restart_in_progress) {
        gfxExit();
    }
    if (!g_exit_requested && !app_system_closing() && !g_playback_restart_in_progress) {
        ui_graphics_init();
    }
}

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
}

static const int *quality_levels(int *count)
{
    if (g_is_new_3ds) {
        *count = (int)(sizeof(QUALITY_LEVELS_NEW3DS) / sizeof(QUALITY_LEVELS_NEW3DS[0]));
        return QUALITY_LEVELS_NEW3DS;
    }
    *count = (int)(sizeof(QUALITY_LEVELS_OLD3DS) / sizeof(QUALITY_LEVELS_OLD3DS[0]));
    return QUALITY_LEVELS_OLD3DS;
}

static const int *audio_rate_levels(int *count)
{
    if (g_is_new_3ds) {
        *count = (int)(sizeof(AUDIO_RATE_LEVELS_NEW3DS) / sizeof(AUDIO_RATE_LEVELS_NEW3DS[0]));
        return AUDIO_RATE_LEVELS_NEW3DS;
    }
    *count = (int)(sizeof(AUDIO_RATE_LEVELS_OLD3DS) / sizeof(AUDIO_RATE_LEVELS_OLD3DS[0]));
    return AUDIO_RATE_LEVELS_OLD3DS;
}

static int quality_index(int quality)
{
    int count = 0;
    const int *levels = quality_levels(&count);
    for (int i = 0; i < count; i++) {
        if (levels[i] == quality) {
            return i;
        }
    }
    return -1;
}

static int audio_rate_index(int rate)
{
    int count = 0;
    const int *levels = audio_rate_levels(&count);
    for (int i = 0; i < count; i++) {
        if (levels[i] == rate) {
            return i;
        }
    }
    return -1;
}

static int bottom_dim_index(int seconds)
{
    for (int i = 0; i < (int)(sizeof(BOTTOM_DIM_LEVELS) / sizeof(BOTTOM_DIM_LEVELS[0])); i++) {
        if (BOTTOM_DIM_LEVELS[i] == seconds) {
            return i;
        }
    }
    return -1;
}

static bool is_supported_quality(int quality)
{
    return quality_index(quality) >= 0;
}

static int default_audio_sample_rate(void)
{
    return g_is_new_3ds ? AUDIO_SAMPLE_RATE_DEFAULT_NEW3DS : AUDIO_SAMPLE_RATE_DEFAULT_OLD3DS;
}

static int settings_audio_sample_rate_normalized(int rate)
{
    return audio_rate_index(rate) >= 0 ? rate : default_audio_sample_rate();
}

static int settings_bottom_dim_normalized_seconds(int seconds)
{
    return bottom_dim_index(seconds) >= 0 ? seconds : BOTTOM_DIM_DEFAULT_SECONDS;
}

static void format_audio_rate_label(char *out, size_t outsz, int rate)
{
    if (!out || outsz == 0) {
        return;
    }
    snprintf(out, outsz, "%d Hz", settings_audio_sample_rate_normalized(rate));
}

static const char *playback_mode_label(int mode)
{
    switch (mode) {
    case PLAYBACK_MODE_H264:
        return "H.264 New3DS";
    case PLAYBACK_MODE_MJPEG:
        return "MJPEG";
    case PLAYBACK_MODE_AUTO:
    default:
        return "Auto";
    }
}

static bool is_supported_playback_mode(int mode)
{
    return mode >= PLAYBACK_MODE_AUTO && mode <= PLAYBACK_MODE_MJPEG;
}

static int settings_buffer_normalized_kb(int kb)
{
    if (kb < SETTINGS_BUFFER_KB_MIN || kb > SETTINGS_BUFFER_KB_MAX) {
        return SETTINGS_BUFFER_KB_DEFAULT;
    }
    if ((kb - SETTINGS_BUFFER_KB_MIN) % SETTINGS_BUFFER_KB_STEP != 0) {
        return SETTINGS_BUFFER_KB_DEFAULT;
    }
    return kb;
}

static int default_quality(void)
{
    return g_is_new_3ds ? 240 : 144;
}

static int quality_display_height(int quality)
{
    return quality == 241 ? 240 : quality;
}

static void format_quality_label(char *out, size_t outsz, int quality)
{
    if (!out || outsz == 0) {
        return;
    }
    if (quality == 241) {
        snprintf(out, outsz, "240HQ");
    } else {
        snprintf(out, outsz, "%dP", quality);
    }
}

static QualityProfile quality_profile(void)
{
    switch (g_cfg.quality) {
    case 144: {
        QualityProfile q = {256, 144, 420000, 48000, 24};
        return q;
    }
    case 360: {
        QualityProfile q = {640, 360, 700000, 96000, 24};
        return q;
    }
    case 480: {
        QualityProfile q = {854, 480, 1200000, 128000, 24};
        return q;
    }
    case 241: {
        QualityProfile q = {400, 240, 1100000, 64000, 24};
        return q;
    }
    case 240:
    default: {
        QualityProfile q = {400, 240, g_is_new_3ds ? 820000 : 720000, 64000, 24};
        return q;
    }
    }
}

static int mjpeg_target_fps(void)
{
    if (g_is_new_3ds) {
        switch (g_cfg.quality) {
        case 144:
            return 15;
        case 360:
            return 10;
        case 480:
            return 8;
        case 240:
        default:
            return 12;
        }
    }
    switch (g_cfg.quality) {
    case 241:
        return 10;
    case 360:
        return 8;
    case 480:
        return 6;
    case 144:
    case 240:
    default:
        return 12;
    }
}

static int mjpeg_target_bitrate(void)
{
    if (g_is_new_3ds) {
        switch (g_cfg.quality) {
        case 144:
            return 520000;
        case 360:
            return 1100000;
        case 480:
            return 1600000;
        case 240:
        default:
            return 760000;
        }
    }
    switch (g_cfg.quality) {
    case 144:
        return 420000;
    case 241:
        return 1100000;
    case 360:
        return 850000;
    case 480:
        return 1200000;
    case 240:
    default:
        return 720000;
    }
}

static int mjpeg_target_audio_sample_rate(void)
{
    return (g_cfg.quality == 240 || g_cfg.quality == 241) ?
        settings_audio_sample_rate_normalized(g_cfg.audio_sample_rate) : AUDIO_SAMPLE_RATE;
}

static void detect_hardware(void)
{
    bool is_new = false;
    g_is_new_3ds = R_SUCCEEDED(APT_CheckNew3DS(&is_new)) && is_new;
}

static void enable_playback_performance_mode(void)
{
    if (g_performance_mode_checked) {
        return;
    }

    g_performance_mode_checked = true;
    g_performance_mode_result = APT_SetAppCpuTimeLimit(STREAM_READER_CPU_TIME_LIMIT);
    g_core1_reader_available = R_SUCCEEDED(g_performance_mode_result);

    if (g_is_new_3ds) {
        osSetSpeedupEnable(true);
    }
}

static void apply_hardware_defaults(void)
{
    bool changed = false;
    if (!g_is_new_3ds && (g_cfg.quality == 360 || g_cfg.quality == 480)) {
        g_cfg.quality = 241;
        changed = true;
    }
    if (!is_supported_quality(g_cfg.quality)) {
        g_cfg.quality = default_quality();
        changed = true;
    }
    if (!is_supported_playback_mode(g_cfg.playback_mode)) {
        g_cfg.playback_mode = PLAYBACK_MODE_AUTO;
        changed = true;
    }
    int buffer_kb = settings_buffer_normalized_kb(g_cfg.stream_buffer_kb);
    if (g_cfg.stream_buffer_kb != buffer_kb) {
        g_cfg.stream_buffer_kb = buffer_kb;
        changed = true;
    }
    int audio_rate = settings_audio_sample_rate_normalized(g_cfg.audio_sample_rate);
    if (g_cfg.audio_sample_rate != audio_rate) {
        g_cfg.audio_sample_rate = audio_rate;
        changed = true;
    }
    int bottom_dim_seconds = settings_bottom_dim_normalized_seconds(g_cfg.bottom_dim_seconds);
    if (g_cfg.bottom_dim_seconds != bottom_dim_seconds) {
        g_cfg.bottom_dim_seconds = bottom_dim_seconds;
        changed = true;
    }
    if (changed) {
        save_config();
    }
}
