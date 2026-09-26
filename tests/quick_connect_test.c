#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define APP_VERSION "test"
static struct { char server[256], username[96], password[96], token[192], user_id[80], device_id[80]; } g_cfg;
static bool g_session_capabilities_reported;
static int saves, libraries;
static u64 now = 1000;
static char status_text[512];
static u64 osGetTime(void) { return now; }
static void save_config(void) { saves++; }
static bool load_libraries(void) { libraries++; return true; }
static bool curl_http_runtime_init(void) { return true; }
static void set_status(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    vsnprintf(status_text, sizeof(status_text), fmt, args); va_end(args);
}
static void setup_log(const char *fmt, ...) { (void)fmt; }
/* Generated from the production parser, URL builder and auth header functions. */
#include "quick_connect_support.inc"
#include "../source/parts/auth_session.inc"
#include "../source/parts/quick_connect.inc"

static void reset(const char *base, const char *scenario)
{
    quick_connect_cancel();
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.server, sizeof(g_cfg.server), "%s/jellyfin/%s", base, scenario);
    strcpy(g_cfg.username, "old-user");
    strcpy(g_cfg.password, "old-password");
    strcpy(g_cfg.user_id, "old-id");
    strcpy(g_cfg.token, "old-token");
    strcpy(g_cfg.device_id, "3dJelly-host-test");
    saves = libraries = 0;
    now = 1000;
}

static void run_to_end(void)
{
    for (int frame = 0; frame < 12000 && g_qc_state != QC_OFF && g_qc_state != QC_ERROR; frame++) {
        quick_connect_tick();
        now += 100;
        usleep(1000);
    }
    assert(g_qc_state == QC_OFF || g_qc_state == QC_ERROR);
}

static size_t discard_response(void *data, size_t size, size_t count, void *unused)
{
    (void)data; (void)unused;
    return size * count;
}

/* Optional hardware-server check. Only the user-facing code is printed. */
static int live_check(const char *server)
{
    copy_safe(g_cfg.server, sizeof(g_cfg.server), server);
    copy_safe(g_cfg.device_id, sizeof(g_cfg.device_id), "3dJelly-quick-connect-verification");
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    quick_connect_start();
    bool printed = false;
    while (g_qc_state != QC_OFF && g_qc_state != QC_ERROR) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        quick_connect_tick();
        if (g_qc_code[0] && !printed) {
            printf("LIVE QUICK CONNECT CODE: %s\n", g_qc_code);
            printed = true;
        }
        usleep(20000);
    }
    if (g_qc_state != QC_OFF || saves != 1) {
        fprintf(stderr, "Live check: %s\n", status_text);
        return 1;
    }
    CURL *http = curl_easy_init();
    assert(http);
    char auth[448], header[480], url[800];
    auth_header(auth, sizeof(auth), true);
    snprintf(header, sizeof(header), "Authorization: %s", auth);
    struct curl_slist *headers = curl_slist_append(NULL, header);
    curl_easy_setopt(http, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(http, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(http, CURLOPT_WRITEFUNCTION, discard_response);
    build_url(url, sizeof(url), "/Users/Me");
    curl_easy_setopt(http, CURLOPT_URL, url);
    CURLcode result = curl_easy_perform(http);
    long status = 0;
    curl_easy_getinfo(http, CURLINFO_RESPONSE_CODE, &status);
    bool ok = result == CURLE_OK && status == 200;
    /* Revoke the temporary verification session, leaving the 3DS session alone. */
    build_url(url, sizeof(url), "/Sessions/Logout");
    curl_easy_setopt(http, CURLOPT_URL, url);
    curl_easy_setopt(http, CURLOPT_POSTFIELDS, "");
    result = curl_easy_perform(http);
    curl_easy_getinfo(http, CURLINFO_RESPONSE_CODE, &status);
    ok = ok && result == CURLE_OK && status >= 200 && status < 300;
    curl_easy_cleanup(http);
    curl_slist_free_all(headers);
    memset(g_cfg.token, 0, sizeof(g_cfg.token));
    puts(ok ? "PASS: real Jellyfin approval, token exchange, authenticated request and test-session logout" : "FAIL: live authenticated request or test-session logout");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc == 2 || (argc == 3 && !strcmp(argv[2], "--live")));
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    if (argc == 3) {
        int result = live_check(argv[1]);
        curl_global_cleanup();
        return result;
    }
    const char *success[] = {"happy", "poll-retry", "auth-retry", "redirect"};
    for (unsigned i = 0; i < sizeof(success) / sizeof(*success); i++) {
        reset(argv[1], success[i]);
        quick_connect_start();
        run_to_end();
        if (g_qc_state != QC_OFF) fprintf(stderr, "%s: %s\n", success[i], status_text);
        assert(g_qc_state == QC_OFF && saves == 1 && libraries == 1);
        assert(!strcmp(g_cfg.username, "Quick User"));
        assert(!strcmp(g_cfg.user_id, "0123456789abcdef0123456789abcdef"));
        assert(!strcmp(g_cfg.token, "new-session-token"));
        assert(!g_cfg.password[0] && !g_qc_secret[0] && !g_qc_code[0]);
        assert(!g_qc_http && !g_qc_multi && !g_qc_response);
        printf("PASS: %s\n", success[i]);
    }
    const char *failure[] = {"disabled", "unsupported", "forbidden", "expired", "bad-init",
        "bad-auth", "bad-enabled", "bad-poll", "poll-fails", "oversize", "truncated-auth"};
    for (unsigned i = 0; i < sizeof(failure) / sizeof(*failure); i++) {
        reset(argv[1], failure[i]);
        quick_connect_start();
        run_to_end();
        assert(g_qc_state == QC_ERROR && saves == 0 && libraries == 0);
        assert(!strcmp(g_cfg.token, "old-token") && !strcmp(g_cfg.user_id, "old-id"));
        assert(!strcmp(g_cfg.password, "old-password"));
        assert(!g_qc_http && !g_qc_multi && !g_qc_response && !g_qc_secret[0] && !g_qc_code[0]);
        printf("PASS: %s preserves saved session\n", failure[i]);
    }
    reset(argv[1], "happy");
    quick_connect_start();
    quick_connect_cancel();
    quick_connect_tick();
    assert(g_qc_state == QC_OFF && !g_qc_http && saves == 0);
    puts("PASS: cancel in-flight request");

    quick_connect_start();
    strcpy(g_cfg.server, "http://changed-server");
    quick_connect_tick();
    assert(g_qc_state == QC_OFF && !g_qc_secret[0] && saves == 0);
    puts("PASS: changing server cancels old request");

    reset(argv[1], "expiry-clock");
    quick_connect_start();
    for (int frame = 0; frame < 2000 && g_qc_state != QC_WAIT; frame++) {
        quick_connect_tick(); now++; usleep(1000);
    }
    assert(g_qc_state == QC_WAIT && !strcmp(g_qc_code, "012345"));
    now = g_qc_deadline;
    quick_connect_tick();
    assert(g_qc_state == QC_ERROR && !g_qc_secret[0] && !g_qc_code[0] && saves == 0);
    puts("PASS: leading-zero code and ten-minute expiry");
    quick_connect_start();
    assert(g_qc_state == QC_CHECK && !g_qc_secret[0]);
    quick_connect_cancel();
    puts("PASS: retry starts a fresh request");

    const char *password_auth = "{\"AccessToken\":\"password-token\",\"User\":{\"Id\":\"password-user-id\",\"Name\":\"Password User\"}}";
    assert(accept_authentication_response(password_auth, strlen(password_auth), false));
    assert(!strcmp(g_cfg.password, "old-password") && !strcmp(g_cfg.token, "password-token"));
    puts("PASS: password login keeps its password");
    curl_global_cleanup();
    return 0;
}
