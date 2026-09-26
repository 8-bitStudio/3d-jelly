/* Jellyfin Quick Connect: https://jellyfin.org/docs/general/server/quick-connect/
   The public code is displayed; the private secret stays in memory only. */
typedef enum { QC_OFF, QC_CHECK, QC_INITIATE, QC_WAIT, QC_POLL, QC_AUTH, QC_ERROR } QuickConnectState;
static QuickConnectState g_qc_state;
static CURLM *g_qc_multi;
static CURL *g_qc_http;
static struct curl_slist *g_qc_headers;
static char *g_qc_response;
static size_t g_qc_response_size;
static char g_qc_secret[128], g_qc_code[7], g_qc_server[256];
static u64 g_qc_deadline, g_qc_next_request;
static int g_qc_failures;
static bool g_qc_retry_selected;
#define QC_RESPONSE_CAP (64 * 1024)

static void quick_connect_close_http(void)
{
    if (g_qc_http) {
        if (g_qc_multi) curl_multi_remove_handle(g_qc_multi, g_qc_http);
        curl_easy_cleanup(g_qc_http);
    }
    if (g_qc_multi) curl_multi_cleanup(g_qc_multi);
    curl_slist_free_all(g_qc_headers);
    g_qc_http = NULL;
    g_qc_multi = NULL;
    g_qc_headers = NULL;
}

static void quick_connect_clear_response(void)
{
    if (g_qc_response) memset(g_qc_response, 0, g_qc_response_size);
    free(g_qc_response);
    g_qc_response = NULL;
    g_qc_response_size = 0;
}

static void quick_connect_cancel(void)
{
    quick_connect_close_http();
    quick_connect_clear_response();
    memset(g_qc_secret, 0, sizeof(g_qc_secret));
    memset(g_qc_code, 0, sizeof(g_qc_code));
    g_qc_server[0] = 0;
    g_qc_deadline = g_qc_next_request = 0;
    g_qc_failures = 0;
    g_qc_state = QC_OFF;
}

static void quick_connect_fail(const char *message)
{
    quick_connect_cancel();
    g_qc_state = QC_ERROR;
    g_qc_retry_selected = true;
    set_status("%s", message);
}

static size_t quick_connect_receive(void *data, size_t size, size_t count, void *unused)
{
    (void)unused;
    if (size && count > QC_RESPONSE_CAP / size) return 0;
    size_t n = size * count;
    if (n > QC_RESPONSE_CAP - g_qc_response_size) return 0;
    char *next = realloc(g_qc_response, g_qc_response_size + n + 1);
    if (!next) return 0;
    g_qc_response = next;
    memcpy(next + g_qc_response_size, data, n);
    g_qc_response_size += n;
    next[g_qc_response_size] = 0;
    return n;
}

static bool quick_connect_request(QuickConnectState operation)
{
    quick_connect_close_http();
    quick_connect_clear_response();
    if (!curl_http_runtime_init()) goto failure;
    g_qc_multi = curl_multi_init();
    g_qc_http = curl_easy_init();
    if (!g_qc_multi || !g_qc_http) goto failure;
    char path[512], url[800], auth[384], header[416];
    if (operation == QC_CHECK) copy_safe(path, sizeof(path), "/QuickConnect/Enabled");
    else if (operation == QC_INITIATE) copy_safe(path, sizeof(path), "/QuickConnect/Initiate");
    else if (operation == QC_AUTH) copy_safe(path, sizeof(path), "/Users/AuthenticateWithQuickConnect");
    else {
        char escaped[384];
        url_encode(g_qc_secret, escaped, sizeof(escaped));
        snprintf(path, sizeof(path), "/QuickConnect/Connect?secret=%s", escaped);
    }
    build_url(url, sizeof(url), path);
    auth_header(auth, sizeof(auth), false);
    snprintf(header, sizeof(header), "Authorization: %s", auth);
    g_qc_headers = curl_slist_append(NULL, header);
    if (!g_qc_headers) goto failure;
    struct curl_slist *headers = curl_slist_append(g_qc_headers, "Content-Type: application/json");
    if (!headers) goto failure;
    g_qc_headers = headers;
    curl_easy_setopt(g_qc_http, CURLOPT_URL, url);
    curl_easy_setopt(g_qc_http, CURLOPT_HTTPHEADER, g_qc_headers);
    curl_easy_setopt(g_qc_http, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(g_qc_http, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(g_qc_http, CURLOPT_POSTREDIR, (long)CURL_REDIR_POST_ALL);
    curl_easy_setopt(g_qc_http, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(g_qc_http, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(g_qc_http, CURLOPT_USERAGENT, "3dJelly/" APP_VERSION);
    curl_easy_setopt(g_qc_http, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(g_qc_http, CURLOPT_TIMEOUT_MS, 8000L);
    /* Match the application's existing TLS policy. */
    curl_easy_setopt(g_qc_http, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(g_qc_http, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(g_qc_http, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_qc_http, CURLOPT_WRITEFUNCTION, quick_connect_receive);
    if (operation == QC_INITIATE || operation == QC_AUTH) {
        char body[288], escaped[256];
        json_escape(g_qc_secret, escaped, sizeof(escaped));
        snprintf(body, sizeof(body), "{\"Secret\":\"%s\"}", escaped);
        curl_easy_setopt(g_qc_http, CURLOPT_POST, 1L);
        curl_easy_setopt(g_qc_http, CURLOPT_COPYPOSTFIELDS, operation == QC_AUTH ? body : "{}");
    }
    if (curl_multi_add_handle(g_qc_multi, g_qc_http) != CURLM_OK) goto failure;
    g_qc_state = operation;
    return true;
failure:
    quick_connect_fail("Could not start Quick Connect. Check Wi-Fi and try again.");
    return false;
}

static void quick_connect_start(void)
{
    quick_connect_cancel();
    copy_safe(g_qc_server, sizeof(g_qc_server), g_cfg.server);
    g_qc_retry_selected = false;
    set_status("Checking Quick Connect on your server...");
    quick_connect_request(QC_CHECK);
}

static void quick_connect_complete(CURLcode result, long status)
{
    QuickConnectState operation = g_qc_state;
    quick_connect_close_http();
    /* Log only result codes, never the secret, public code or session token. */
    setup_log("Quick Connect: operation=%d curl=%d http=%ld", operation, (int)result, status);
    if (result != CURLE_OK || status >= 500 || status == 429) {
        if ((operation == QC_POLL || operation == QC_AUTH) && ++g_qc_failures <= 3) {
            quick_connect_clear_response();
            g_qc_next_request = osGetTime() + 3000;
            set_status("Connection interrupted. Retrying Quick Connect...");
        } else quick_connect_fail("Quick Connect could not reach the server. Check Wi-Fi and retry.");
        return;
    }
    if (status == 401 || status == 403) {
        quick_connect_fail("Quick Connect is disabled or not allowed. Use User and Pass instead.");
        return;
    }
    if ((operation == QC_POLL || operation == QC_AUTH) && (status == 404 || status == 410)) {
        quick_connect_fail("This code expired or is no longer valid. Choose Try Again for a new code.");
        return;
    }
    if (status == 404 || status == 405) {
        quick_connect_fail("This server does not support Quick Connect. Use User and Pass instead.");
        return;
    }
    if (status != 200 || !g_qc_response) {
        quick_connect_fail("The server could not complete Quick Connect. Try again or sign in manually.");
        return;
    }
    g_qc_failures = 0;
    const char *body = g_qc_response, *end = body + g_qc_response_size;
    if (operation == QC_CHECK) {
        const char *value = skip_ws(body, end);
        if (end - value >= 5 && !memcmp(value, "false", 5) && skip_ws(value + 5, end) == end) {
            quick_connect_fail("Quick Connect is disabled on this server. Use User and Pass instead.");
            return;
        }
        if (end - value < 4 || memcmp(value, "true", 4) || skip_ws(value + 4, end) != end) {
            quick_connect_fail("The server returned an invalid Quick Connect response.");
            return;
        }
        g_qc_deadline = osGetTime() + 10 * 60 * 1000;
        set_status("Requesting a Quick Connect code...");
        quick_connect_request(QC_INITIATE);
        return;
    }
    if (operation == QC_AUTH) {
        if (!accept_authentication_response(body, g_qc_response_size, true)) {
            quick_connect_fail("Sign-in response was incomplete. Please try Quick Connect again.");
            return;
        }
        quick_connect_cancel();
        set_status("Signed in with Quick Connect. Opening your libraries...");
        load_libraries();
        return;
    }
    const char *root, *root_end;
    bool authenticated = false;
    if (!json_object_range_after(body, end, &root, &root_end) || skip_ws(root_end, end) != end ||
        !json_get_bool_range(root, root_end, "Authenticated", &authenticated)) {
        quick_connect_fail("The server returned an invalid Quick Connect response.");
        return;
    }
    if (operation == QC_INITIATE) {
        char code[16];
        if (!json_get_string_range(root, root_end, "Secret", g_qc_secret, sizeof(g_qc_secret)) ||
            !json_get_string_range(root, root_end, "Code", code, sizeof(code)) ||
            strlen(code) != 6 || strspn(code, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != 6 ||
            !g_qc_secret[0] || strlen(g_qc_secret) >= sizeof(g_qc_secret) - 1 ||
            strspn(g_qc_secret, "0123456789abcdefABCDEF") != strlen(g_qc_secret)) {
            quick_connect_fail("The server did not return a valid Quick Connect code. Try again.");
            return;
        }
        copy_safe(g_qc_code, sizeof(g_qc_code), code);
    }
    quick_connect_clear_response();
    if (authenticated) {
        set_status("Code approved. Signing in...");
        quick_connect_request(QC_AUTH);
    } else {
        g_qc_state = QC_WAIT;
        g_qc_next_request = osGetTime() + 2000;
        set_status("Waiting for approval from your signed-in Jellyfin device...");
    }
}

static void quick_connect_tick(void)
{
    if (g_qc_state == QC_OFF || g_qc_state == QC_ERROR) return;
    if (strcmp(g_qc_server, g_cfg.server)) { quick_connect_cancel(); return; }
    if (g_qc_deadline && osGetTime() >= g_qc_deadline) {
        quick_connect_fail("This code expired. Choose Try Again for a new Quick Connect code.");
        return;
    }
    if (!g_qc_http) {
        if (osGetTime() >= g_qc_next_request)
            quick_connect_request(g_qc_state == QC_WAIT ? QC_POLL : g_qc_state);
        return;
    }
    int running, remaining;
    CURLMcode result = curl_multi_perform(g_qc_multi, &running);
    CURLMsg *message = curl_multi_info_read(g_qc_multi, &remaining);
    if (!message && result == CURLM_OK) return;
    long status = 0;
    curl_easy_getinfo(g_qc_http, CURLINFO_RESPONSE_CODE, &status);
    CURLcode code = message && message->msg == CURLMSG_DONE ? message->data.result : CURLE_RECV_ERROR;
    quick_connect_complete(code, status);
}
