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

static bool is_supported_quality(int quality)
{
    return quality_index(quality) >= 0;
}

static int default_quality(void)
{
    return g_is_new_3ds ? 240 : 144;
}

static int quality_display_height(int quality)
{
    return (quality == 241 || quality == 242) ? 240 : quality;
}

static void format_quality_label(char *out, size_t outsz, int quality)
{
    if (!out || outsz == 0) {
        return;
    }
    if (quality == 241) {
        snprintf(out, outsz, "240HQ");
    } else if (quality == 242) {
        snprintf(out, outsz, "240M1");
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
    case 242: {
        QualityProfile q = {400, 240, 520000, 64000, 24};
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
    case 242:
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
    case 242:
        return 520000;
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
    if (!g_is_new_3ds && (g_cfg.quality == 360 || g_cfg.quality == 480)) {
        g_cfg.quality = 241;
        save_config();
        return;
    }
    if (!is_supported_quality(g_cfg.quality)) {
        g_cfg.quality = default_quality();
        save_config();
    }
}
