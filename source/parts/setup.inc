/* Jellyfin LAN discovery and touch-first sign-in. Included after UI helpers. */
#include "setup_socket.inc"
static void quick_connect_cancel(void);

typedef struct {
    char id[96];
    char name[128];
    char address[256];
} SetupServer;

static SetupServer g_setup_servers[16];
static int g_setup_server_count;
static int g_setup_server_row;
static int g_setup_server_scroll;
static int g_setup_socket = -1;
static u64 g_setup_scan_until;
static u64 g_setup_probe_at;
static struct in_addr g_setup_broadcast;
static u32 g_setup_probe_host, g_setup_probe_end;
static int g_setup_probe_errors;
static bool g_setup_started;
static bool g_setup_choose_server;
static C3D_Tex g_setup_wallpaper;
static bool g_setup_wallpaper_ready;
static char g_setup_wallpaper_server[256];
static Tex3DS_SubTexture g_setup_wallpaper_sub = {400, 240, 0, 1, 400.0f / 512, 1 - 240.0f / 256};

static void setup_log(const char *fmt, ...)
{
    FILE *f = fopen(CONFIG_DIR "/setup-network.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static void setup_socket_failure(const char *operation)
{
    int error = errno;
    setup_log("Discovery %s failed: errno=%d", operation, error);
    set_status("Search %s failed (%d). Press X to retry.", operation, error);
}

static void setup_stop_scan(void)
{
    if (g_setup_socket >= 0) close(g_setup_socket);
    g_setup_socket = -1;
}

static void setup_send_probe(struct in_addr address)
{
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(7359);
    dest.sin_addr = address;
    const char probe[] = "who is JellyfinServer?";
    if (sendto(g_setup_socket, probe, sizeof(probe) - 1, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0 && g_setup_probe_errors++ == 0)
        setup_log("Discovery send failed: errno=%d", errno);
}

static void setup_start_scan(void)
{
    quick_connect_cancel();
    g_view = VIEW_SETUP;
    setup_stop_scan();
    g_setup_choose_server = true;
    g_setup_server_count = g_setup_server_row = g_setup_server_scroll = 0;
    if (!curl_http_runtime_init()) {
        setup_log("Discovery runtime failed: result=%08lX", (unsigned long)g_curl_init_result);
        set_status("Wi-Fi unavailable. You can still enter a server address.");
        return;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        setup_socket_failure("socket");
        return;
    }
    /* libctru defines SO_BROADCAST as 0: broadcasting needs no option on 3DS.
       Passing that placeholder to the service fails. Keep the option only for
       platforms that actually implement it (including the host test). */
#if SO_BROADCAST != 0
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
        setup_socket_failure("broadcast");
        close(fd);
        return;
    }
#endif
    if (setup_bind_scan_socket(fd, (unsigned int)osGetTime()) < 0) {
        setup_socket_failure("bind");
        close(fd);
        return;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        setup_socket_failure("nonblocking");
        close(fd);
        return;
    }
    struct in_addr ip = {0}, mask = {0};
    g_setup_broadcast.s_addr = INADDR_BROADCAST;
    g_setup_probe_host = g_setup_probe_end = 0;
    if (SOCU_GetIPInfo(&ip, &mask, &g_setup_broadcast) == 0 && ip.s_addr && mask.s_addr) {
        u32 host = ntohl(ip.s_addr), netmask = ntohl(mask.s_addr);
        /* Probe the actual subnet up to 1024 addresses; on larger networks,
           probe the device's local /24 in addition to subnet broadcasts. */
        if ((~netmask) > 1023) netmask = 0xFFFFFF00u;
        g_setup_probe_host = (host & netmask) + 1;
        g_setup_probe_end = (host | ~netmask);
    }
    g_setup_socket = fd;
    g_setup_probe_errors = 0;
    setup_log("Discovery started; direct probe range=%08lX..%08lX",
              (unsigned long)g_setup_probe_host, (unsigned long)g_setup_probe_end);
    g_setup_scan_until = osGetTime() + 5500;
    g_setup_probe_at = 0;
    set_status("Searching for Jellyfin servers on your Wi-Fi...");
}

static void setup_poll_scan(void)
{
    if (g_setup_socket < 0) return;
    u64 now = osGetTime();
    if (now >= g_setup_probe_at) {
        setup_send_probe(g_setup_broadcast);
        struct in_addr broadcast = { .s_addr = INADDR_BROADCAST };
        if (broadcast.s_addr != g_setup_broadcast.s_addr) setup_send_probe(broadcast);
        g_setup_probe_at = now + 1000;
    }
    /* Some access points suppress broadcasts between Wi-Fi clients. Send the
       same Jellyfin UDP query directly as a fallback, without blocking frames. */
    for (int i = 0; i < 8 && g_setup_probe_host < g_setup_probe_end; i++) {
        struct in_addr target = { .s_addr = htonl(g_setup_probe_host++) };
        setup_send_probe(target);
    }
    /* Bound work per frame even if the network sends many replies. */
    for (int i = 0; i < 8; i++) {
        char buf[2048];
        struct sockaddr_in sender;
        socklen_t sender_size = sizeof(sender);
        int n = recvfrom(g_setup_socket, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&sender, &sender_size);
        if (n < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                setup_socket_failure("receive");
                setup_stop_scan();
            }
            break;
        }
        if (n == 0 || n == sizeof(buf) - 1) continue;
        buf[n] = 0;
        SetupServer server = {0};
        if (!json_get_string_range(buf, buf + n, "Id", server.id, sizeof(server.id)) ||
            !json_get_string_range(buf, buf + n, "Address", server.address, sizeof(server.address)) ||
            !server.id[0] || !starts_with_http(server.address)) continue;
        json_get_string_range(buf, buf + n, "Name", server.name, sizeof(server.name));
        normalize_server_url(server.address);
        bool duplicate = false;
        for (int j = 0; j < g_setup_server_count; j++) {
            if (!strcmp(server.id, g_setup_servers[j].id) ||
                !strcmp(server.address, g_setup_servers[j].address)) duplicate = true;
        }
        if (!duplicate && g_setup_server_count < 16) {
            g_setup_servers[g_setup_server_count++] = server;
            if (g_setup_server_count == 1) g_setup_server_row = 0;
        }
    }
    if (g_setup_socket >= 0 && osGetTime() >= g_setup_scan_until) {
        setup_log("Discovery complete: servers=%d send_errors=%d", g_setup_server_count, g_setup_probe_errors);
        setup_stop_scan();
        set_status(g_setup_server_count ? "Choose a server to sign in." :
                   "No servers found. Press X to retry, or add a server manually.");
    }
}

#include "setup_assets.inc"
#include "quick_connect.inc"

static void setup_background(bool bottom)
{
    float w = bottom ? 320 : 400;
    C2D_DrawRectSolid(0, 0, 0, w, 240, 0xFF111111);
    if (!g_setup_choose_server && g_setup_wallpaper_ready) {
        C2D_Image image = {&g_setup_wallpaper, &g_setup_wallpaper_sub};
        C2D_DrawImageAt(image, bottom ? -40 : 0, 0, 0, NULL, 1, 1);
        C2D_DrawRectSolid(0, 0, 0, w, 240, bottom ? 0xB8000000 : 0x80000000);
    }
}

static void draw_connecting(bool bottom)
{
    float center = bottom ? 160 : 200;
    C2D_DrawRectSolid(0, 0, 0, bottom ? 320 : 400, 240, 0xFF111111);
    if (bottom) {
        draw_text_centered(center, 103, 0.52f, COL_MUTED, "Opening your server...");
        draw_text_centered(center, 202, 0.42f, COL_MUTED, "B: Open sign-in");
    } else {
        draw_text_centered(center, 87, 1.05f, COL_WHITE, "3dJelly");
    }
}

static void draw_setup(void)
{
    setup_background(false);
    if (g_qc_state != QC_OFF) {
        draw_text_centered(200, 23, 0.9f, COL_WHITE, "Quick Connect");
        if (g_qc_code[0]) {
            draw_text_centered(200, 69, 1.65f, COL_WHITE, "%s", g_qc_code);
            draw_text_centered(200, 120, 0.45f, COL_WHITE, "Enter this code on your signed-in device.");
        } else draw_text_centered(200, 93, 0.55f, COL_WHITE,
            g_qc_state == QC_ERROR ? "Unable to sign in" : "Connecting to your server...");
        C2D_DrawRectSolid(12, 170, 0, 376, 60, 0xC0111111);
        draw_text_wrap(24, 178, 0.43f, 350, COL_WHITE, "%s", g_status);
        return;
    }
    draw_text_centered(200, 45, 1.05f, COL_WHITE, "3dJelly");
    draw_text_centered(200, 91, 0.65f, COL_WHITE,
                       "%s", g_setup_choose_server ? "Select Server" : "Please sign in");
    char address[128];
    clipped_name(g_cfg.server, address, sizeof(address), 55);
    if (text_width_for_scale(address, 0.43f) > 370)
        trim_text_to_width(address, sizeof(address), 0.43f, 370, true);
    draw_text_centered(200, 126, 0.43f, COL_MUTED, "%s",
                       g_setup_choose_server ? "Find Jellyfin on your Wi-Fi" : address);
    C2D_DrawRectSolid(12, 172, 0, 376, 58, 0xC0111111);
    draw_text_wrap(24, 178, 0.43f, 350, COL_WHITE, "%s", g_status);
    if (!g_setup_choose_server && !g_setup_wallpaper_ready) {
        if (g_setup_pixels)
            draw_text_centered(200, 150, 0.38f, COL_MUTED, "Loading wallpaper: %d%%",
                g_setup_mcu * 100 / (g_setup_jpeg.m_MCUSPerRow * g_setup_jpeg.m_MCUSPerCol));
        else if (g_setup_http && !g_setup_check_server)
            draw_text_centered(200, 150, 0.38f, COL_MUTED, "Downloading wallpaper...");
    }
}

static void setup_button(int y, int h, bool selected, bool primary, const char *label)
{
    C2D_DrawRectSolid(20, y, 0, 280, h, selected ? COL_WHITE : 0xFF444444);
    C2D_DrawRectSolid(22, y + 2, 0, 276, h - 4, primary ? 0xFFD0A400 : 0xFF303030);
    draw_text_centered(160, y + (h - 17) / 2, 0.54f, COL_WHITE, "%s", label);
}

static void draw_setup_bottom(void)
{
    setup_background(true);
    if (g_qc_state != QC_OFF) {
        draw_text_centered(160, 8, 0.62f, COL_WHITE, "Use Quick Connect");
        draw_text_wrap(24, 42, 0.47f, 272, COL_WHITE, "%s", g_qc_state == QC_ERROR ?
            "Choose Try Again to request a new code, or Cancel to return to User and Pass sign-in." :
            "On a phone or computer signed in to this Jellyfin server, open Settings > Quick Connect and enter the code above.");
        u64 now = osGetTime();
        if (g_qc_code[0] && g_qc_deadline > now) {
            unsigned seconds = (unsigned)((g_qc_deadline - now) / 1000);
            draw_text_centered(160, 145, 0.42f, COL_MUTED, "Code expires in %u:%02u", seconds / 60, seconds % 60);
        }
        if (g_qc_state == QC_ERROR)
            setup_button(168, 29, g_qc_retry_selected, true, "Try Again  (X)");
        setup_button(203, 29, !g_qc_retry_selected, false, "Cancel  (B)");
        return;
    }
    if (g_setup_choose_server) {
        draw_text_centered(160, 6, 0.62f, COL_WHITE, "Select Server");
        if (g_setup_server_row < g_setup_server_count) {
            if (g_setup_server_row < g_setup_server_scroll) g_setup_server_scroll = g_setup_server_row;
            if (g_setup_server_row >= g_setup_server_scroll + 3) g_setup_server_scroll = g_setup_server_row - 2;
        }
        for (int i = 0; i < 3 && g_setup_server_scroll + i < g_setup_server_count; i++) {
            int idx = g_setup_server_scroll + i;
            int y = 36 + i * 43;
            C2D_DrawRectSolid(20, y, 0, 280, 39, idx == g_setup_server_row ? 0xFFD0A400 : 0xFF404040);
            C2D_DrawRectSolid(22, y + 2, 0, 276, 35, 0xFF252525);
            char text[128];
            clipped_name(g_setup_servers[idx].name[0] ? g_setup_servers[idx].name : "Jellyfin", text, sizeof(text), 33);
            if (text_width_for_scale(text, 0.46f) > 258) trim_text_to_width(text, sizeof(text), 0.46f, 258, true);
            draw_text(30, y + 3, 0.46f, COL_WHITE, "%s", text);
            clipped_name(g_setup_servers[idx].address, text, sizeof(text), 44);
            if (text_width_for_scale(text, 0.34f) > 258) trim_text_to_width(text, sizeof(text), 0.34f, 258, true);
            draw_text(30, y + 22, 0.34f, COL_MUTED, "%s", text);
        }
        if (!g_setup_server_count) draw_text_wrap(30, 63, 0.46f, 260, COL_MUTED, "%s",
            g_setup_socket >= 0 ? "Searching your Wi-Fi..." : "No servers found. Add a server below.");
        setup_button(169, 30, g_setup_server_row == g_setup_server_count, true, "Add Server");
        setup_button(204, 28, g_setup_server_row == g_setup_server_count + 1, false, "Search Again  (X)");
    } else {
        draw_text_centered(160, 6, 0.62f, COL_WHITE, "Please sign in");
        const char *labels[] = {"User", "Pass"};
        for (int i = 0; i < 2; i++) {
            int y = 45 + i * 51;
            draw_text(20, y - 18, 0.47f, COL_WHITE, "%s", labels[i]);
            C2D_DrawRectSolid(20, y, 0, 280, 31, g_setup_row == i ? 0xFFD0A400 : 0xFF444444);
            C2D_DrawRectSolid(22, y + 2, 0, 276, 27, 0xFF252525);
            char value[128];
            clipped_name(i ? (g_cfg.password[0] ? "********" : "") : g_cfg.username, value, sizeof(value), 32);
            if (text_width_for_scale(value, 0.52f) > 258) trim_text_to_width(value, sizeof(value), 0.52f, 258, true);
            draw_text(30, y + 6, 0.52f, COL_WHITE, "%s", value);
        }
        setup_button(136, 30, g_setup_row == 2, true, "Sign In");
        setup_button(170, 28, g_setup_row == 3, false, "Use Quick Connect");
        setup_button(204, 28, g_setup_row == 4, false, "Change Server");
    }
}

static void setup_select_server(const char *address)
{
    quick_connect_cancel();
    setup_cancel_http();
    g_setup_resume_session = false;
    if (strcmp(address, g_cfg.server)) {
        remote_control_stop();
        g_cfg.token[0] = g_cfg.user_id[0] = g_cfg.password[0] = 0;
        g_session_capabilities_reported = false;
        copy_safe(g_cfg.server, sizeof(g_cfg.server), address);
        save_config();
    }
    setup_stop_scan();
    g_setup_choose_server = false;
    g_setup_row = 0;
    set_status("Enter your User and Pass, then Sign In.");
}

static void handle_setup(u32 down)
{
    if (!g_setup_started) {
        g_setup_started = true;
        mkdir(CONFIG_DIR, 0777);
        FILE *log = fopen(CONFIG_DIR "/setup-network.log", "w");
        if (log) { fputs("3dJelly setup: smooth wallpaper and outlined text\n", log); fclose(log); }
        if (!g_cfg.server[0]) setup_start_scan();
        else {
            set_status("Connecting to your server...");
            if (!setup_begin_http(true)) {
                g_setup_resume_session = false;
                setup_start_scan();
            }
        }
    }
    /* Saved sessions have their own startup view. Do not accept touches or A
       on the hidden sign-in controls while the connection is pending. */
    if (g_view == VIEW_CONNECTING) {
        if (down & KEY_B) {
            setup_cancel_http();
            g_setup_resume_session = false;
            g_view = VIEW_SETUP;
            set_status("Enter your User and Pass, then Sign In.");
        }
        return;
    }
    if (g_qc_state != QC_OFF) {
        bool cancel = (down & KEY_B) != 0;
        bool retry = g_qc_state == QC_ERROR && (down & KEY_X);
        if (g_qc_state == QC_ERROR && (down & (KEY_UP | KEY_DOWN)))
            g_qc_retry_selected = !g_qc_retry_selected;
        if (down & KEY_A) {
            if (g_qc_state == QC_ERROR && g_qc_retry_selected) retry = true;
            else cancel = true;
        }
        if (down & KEY_TOUCH) {
            touchPosition p; hidTouchRead(&p);
            if (p.px >= 20 && p.px < 300) {
                if (p.py >= 203 && p.py < 232) cancel = true;
                if (g_qc_state == QC_ERROR && p.py >= 168 && p.py < 197) retry = true;
            }
        }
        if (cancel) {
            quick_connect_cancel();
            g_setup_row = 3;
            set_status("Enter your User and Pass, or use Quick Connect.");
        } else if (retry) quick_connect_start();
        else quick_connect_tick();
        return;
    }
    setup_poll_scan();
    if (!g_cfg.server[0] && !g_setup_choose_server) setup_start_scan();
    bool activate = (down & KEY_A) != 0;
    if (g_setup_choose_server) {
        int count = g_setup_server_count + 2;
        if (down & KEY_DOWN) g_setup_server_row = (g_setup_server_row + 1) % count;
        if (down & KEY_UP) g_setup_server_row = (g_setup_server_row + count - 1) % count;
        if (down & KEY_X) { setup_cancel_http(); setup_start_scan(); return; }
        if ((down & KEY_B) && g_cfg.server[0]) { setup_stop_scan(); g_setup_choose_server = false; return; }
        if (down & KEY_TOUCH) {
            touchPosition p; hidTouchRead(&p);
            if (p.px >= 20 && p.px < 300) {
                int row = (p.py - 36) / 43 + g_setup_server_scroll;
                if (p.py >= 36 && p.py < 165 && row < g_setup_server_count && (p.py - 36) % 43 < 39) {
                    g_setup_server_row = row; activate = true;
                } else if (p.py >= 169 && p.py < 199) { g_setup_server_row = g_setup_server_count; activate = true; }
                else if (p.py >= 204 && p.py < 232) { g_setup_server_row = g_setup_server_count + 1; activate = true; }
            }
        }
        if (!activate) return;
        if (g_setup_server_row < g_setup_server_count) setup_select_server(g_setup_servers[g_setup_server_row].address);
        else if (g_setup_server_row == g_setup_server_count) {
            char address[256]; copy_safe(address, sizeof(address), g_cfg.server);
            if (edit_text(tr(TR_SERVER_URL), address, sizeof(address), false)) {
                normalize_server_url(address);
                if (starts_with_http(address)) setup_select_server(address);
                else set_status("Enter an http:// or https:// server address.");
            }
        } else setup_start_scan();
        return;
    }
    if (down & KEY_DOWN) g_setup_row = (g_setup_row + 1) % 5;
    if (down & KEY_UP) g_setup_row = (g_setup_row + 4) % 5;
    if (down & KEY_B) { setup_cancel_http(); g_setup_resume_session = false; setup_start_scan(); return; }
    if (down & KEY_TOUCH) {
        touchPosition p; hidTouchRead(&p);
        if (p.px >= 20 && p.px < 300) {
            if (p.py >= 45 && p.py < 76) { g_setup_row = 0; activate = true; }
            else if (p.py >= 96 && p.py < 127) { g_setup_row = 1; activate = true; }
            else if (p.py >= 136 && p.py < 166) { g_setup_row = 2; activate = true; }
            else if (p.py >= 170 && p.py < 198) { g_setup_row = 3; activate = true; }
            else if (p.py >= 204 && p.py < 232) { g_setup_row = 4; activate = true; }
        }
    }
    if (!activate) return;
    if (g_setup_row < 2) {
        bool password = g_setup_row == 1;
        if (edit_text(password ? "Pass" : "User", password ? g_cfg.password : g_cfg.username, 96, password)) {
            g_cfg.token[0] = g_cfg.user_id[0] = 0;
            save_config();
            g_setup_row++;
        }
    } else if (g_setup_row == 2) {
        setup_cancel_http();
        g_setup_resume_session = false;
        if (login_jellyfin()) load_libraries();
    } else if (g_setup_row == 3) {
        setup_cancel_http();
        g_setup_resume_session = false;
        setup_stop_scan();
        quick_connect_start();
    } else { setup_cancel_http(); g_setup_resume_session = false; setup_start_scan(); }
}
