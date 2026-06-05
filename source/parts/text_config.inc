static void copy_safe(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool append_char(char *out, size_t outsz, size_t *w, char c)
{
    if (!out || !w || *w + 1 >= outsz) {
        return false;
    }
    out[(*w)++] = c;
    out[*w] = 0;
    return true;
}

static bool append_text(char *out, size_t outsz, size_t *w, const char *text)
{
    for (size_t i = 0; text && text[i]; i++) {
        if (!append_char(out, outsz, w, text[i])) {
            return false;
        }
    }
    return true;
}

static bool append_utf8_codepoint(char *out, size_t outsz, size_t *w, u32 cp)
{
    if (cp <= 0x7F) {
        return append_char(out, outsz, w, (char)cp);
    }
    if (cp <= 0x7FF) {
        if (!out || !w || *w + 2 >= outsz) {
            return false;
        }
        out[(*w)++] = (char)(0xC0 | (cp >> 6));
        out[(*w)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (!out || !w || *w + 3 >= outsz || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        out[(*w)++] = (char)(0xE0 | (cp >> 12));
        out[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*w)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        if (!out || !w || *w + 4 >= outsz) {
            return false;
        }
        out[(*w)++] = (char)(0xF0 | (cp >> 18));
        out[(*w)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*w)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*w)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        return false;
    }
    out[*w] = 0;
    return true;
}

static bool append_hangul_codepoint(char *out, size_t outsz, size_t *w, u32 cp)
{
    static const char *initials[] = {
        "g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s",
        "ss", "", "j", "jj", "ch", "k", "t", "p", "h"
    };
    static const char *vowels[] = {
        "a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa",
        "wae", "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i"
    };
    static const char *finals[] = {
        "", "k", "k", "ks", "n", "nj", "nh", "t", "l", "lk",
        "lm", "lb", "ls", "lt", "lp", "lh", "m", "p", "ps", "t",
        "t", "ng", "t", "t", "k", "t", "p", "t"
    };

    if (cp >= 0xAC00 && cp <= 0xD7A3) {
        u32 s = cp - 0xAC00;
        u32 initial = s / (21 * 28);
        u32 vowel = (s % (21 * 28)) / 28;
        u32 final = s % 28;
        return append_text(out, outsz, w, initials[initial]) &&
               append_text(out, outsz, w, vowels[vowel]) &&
               append_text(out, outsz, w, finals[final]);
    }

    switch (cp) {
    case 0x3131:
    case 0x1100:
        return append_text(out, outsz, w, "g");
    case 0x3132:
    case 0x1101:
        return append_text(out, outsz, w, "kk");
    case 0x3134:
    case 0x1102:
        return append_text(out, outsz, w, "n");
    case 0x3137:
    case 0x1103:
        return append_text(out, outsz, w, "d");
    case 0x3138:
    case 0x1104:
        return append_text(out, outsz, w, "tt");
    case 0x3139:
    case 0x1105:
        return append_text(out, outsz, w, "r");
    case 0x3141:
    case 0x1106:
        return append_text(out, outsz, w, "m");
    case 0x3142:
    case 0x1107:
        return append_text(out, outsz, w, "b");
    case 0x3143:
    case 0x1108:
        return append_text(out, outsz, w, "pp");
    case 0x3145:
    case 0x1109:
        return append_text(out, outsz, w, "s");
    case 0x3146:
    case 0x110A:
        return append_text(out, outsz, w, "ss");
    case 0x3147:
    case 0x110B:
        return append_text(out, outsz, w, "ng");
    case 0x3148:
    case 0x110C:
        return append_text(out, outsz, w, "j");
    case 0x3149:
    case 0x110D:
        return append_text(out, outsz, w, "jj");
    case 0x314A:
    case 0x110E:
        return append_text(out, outsz, w, "ch");
    case 0x314B:
    case 0x110F:
        return append_text(out, outsz, w, "k");
    case 0x314C:
    case 0x1110:
        return append_text(out, outsz, w, "t");
    case 0x314D:
    case 0x1111:
        return append_text(out, outsz, w, "p");
    case 0x314E:
    case 0x1112:
        return append_text(out, outsz, w, "h");
    default:
        return false;
    }
}

static bool append_display_codepoint(char *out, size_t outsz, size_t *w, u32 cp)
{
    if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0x1100 && cp <= 0x11FF)) {
        return append_hangul_codepoint(out, outsz, w, cp);
    }

    switch (cp) {
    case 0x00A0:
        return append_char(out, outsz, w, ' ');
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
    case 0x2032:
        return append_char(out, outsz, w, '\'');
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x201F:
    case 0x2033:
        return append_char(out, outsz, w, '"');
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2212:
        return append_char(out, outsz, w, '-');
    case 0x2026:
        return append_text(out, outsz, w, "...");
    default:
        return append_utf8_codepoint(out, outsz, w, cp);
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool read_json_hex4(const char *p, const char *end, u32 *out)
{
    if (!p || !out || p + 4 > end) {
        return false;
    }
    u32 cp = 0;
    for (int i = 0; i < 4; i++) {
        int v = hex_value(p[i]);
        if (v < 0) {
            return false;
        }
        cp = (cp << 4) | (u32)v;
    }
    *out = cp;
    return true;
}

static size_t read_utf8_codepoint(const char *p, const char *end, u32 *out)
{
    if (!p || !out || p >= end) {
        return 0;
    }

    unsigned char c0 = (unsigned char)p[0];
    if (c0 < 0x80) {
        *out = c0;
        return 1;
    }

    size_t len = 0;
    u32 cp = 0;
    if ((c0 & 0xE0) == 0xC0) {
        len = 2;
        cp = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        len = 3;
        cp = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        len = 4;
        cp = c0 & 0x07;
    } else {
        return 0;
    }

    if (p + len > end) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        unsigned char cx = (unsigned char)p[i];
        if ((cx & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (u32)(cx & 0x3F);
    }
    *out = cp;
    return len;
}

static size_t utf8_sequence_len(const char *s)
{
    unsigned char c = (unsigned char)(s ? s[0] : 0);
    if (c < 0x80) {
        return c ? 1 : 0;
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

static void trim_newline(char *s)
{
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = 0;
    }
}

static void trim_edges(char *s)
{
    if (!s) {
        return;
    }
    trim_newline(s);
    while (*s == ' ' || *s == '\t') {
        memmove(s, s + 1, strlen(s));
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

static void strip_trailing_slash(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') {
        s[--n] = 0;
    }
}

static bool starts_with_http(const char *s);

static void normalize_server_url(char *s)
{
    if (!s || !s[0]) {
        return;
    }

    trim_edges(s);

    if (!starts_with_http(s)) {
        char tmp[320];
        snprintf(tmp, sizeof(tmp), "http://%s", s);
        copy_safe(s, 256, tmp);
    }

    char *q = strchr(s, '?');
    if (q) {
        *q = 0;
    }
    char *hash = strchr(s, '#');
    if (hash) {
        *hash = 0;
    }

    strip_trailing_slash(s);

    const char *suffixes[] = {
        "/web/index.html",
        "/web",
        "/login.html"
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t n = strlen(s);
        size_t m = strlen(suffixes[i]);
        if (n >= m && strcmp(s + n - m, suffixes[i]) == 0) {
            s[n - m] = 0;
            strip_trailing_slash(s);
            break;
        }
    }
}

static bool starts_with_http(const char *s)
{
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

static bool hex_write_bytes(const unsigned char *bytes, size_t len, char *out, size_t outsz, size_t *w)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!bytes || !out || !w) {
        return false;
    }
    if (*w + len * 2 >= outsz) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        out[(*w)++] = hex[bytes[i] >> 4];
        out[(*w)++] = hex[bytes[i] & 0x0F];
    }
    out[*w] = 0;
    return true;
}

static bool hex_read_bytes(const char *hex, unsigned char *out, size_t out_len)
{
    if (!hex || !out) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

static bool hex_read_u64_fixed(const char *hex, size_t digits, u64 *out)
{
    if (!hex || !out || digits > 16) {
        return false;
    }
    u64 v = 0;
    for (size_t i = 0; i < digits; i++) {
        int n = hex_value(hex[i]);
        if (n < 0) {
            return false;
        }
        v = (v << 4) | (u64)n;
    }
    *out = v;
    return true;
}

static void sha256_update_string(mbedtls_sha256_context *ctx, const char *s)
{
    if (s && s[0]) {
        mbedtls_sha256_update_ret(ctx, (const unsigned char *)s, strlen(s));
    }
    const unsigned char zero = 0;
    mbedtls_sha256_update_ret(ctx, &zero, 1);
}

static void sha256_update_u64(mbedtls_sha256_context *ctx, u64 v)
{
    unsigned char b[8];
    for (int i = 0; i < 8; i++) {
        b[7 - i] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
    mbedtls_sha256_update_ret(ctx, b, sizeof(b));
}

static u64 config_console_unique_hash(void)
{
    static bool tried;
    static u64 cached;

    if (!tried) {
        tried = true;
        u64 hash = 0;
        if (R_SUCCEEDED(cfguInit())) {
            if (R_SUCCEEDED(CFGU_GenHashConsoleUnique(0x334A4C59, &hash))) {
                cached = hash;
            }
            cfguExit();
        }
        if (!cached) {
            cached = 0x33444A656C6C7931ULL;
        }
    }

    return cached;
}

static bool config_secret_digest(const char *field, u64 nonce, const char *purpose, int version, bool bind_identity, unsigned char out[32])
{
    const char *salt = version >= 2 ? "3dJelly config secret v2" : "3dJelly config secret v1";
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    bool ok = mbedtls_sha256_starts_ret(&ctx, 0) == 0;
    if (ok) {
        ok = mbedtls_sha256_update_ret(&ctx, (const unsigned char *)salt, strlen(salt)) == 0;
    }
    if (ok) {
        sha256_update_u64(&ctx, config_console_unique_hash());
        sha256_update_u64(&ctx, nonce);
        sha256_update_string(&ctx, purpose);
        sha256_update_string(&ctx, field);
        sha256_update_string(&ctx, g_cfg.device_id);
        if (bind_identity) {
            sha256_update_string(&ctx, g_cfg.server);
            sha256_update_string(&ctx, g_cfg.username);
        }
        ok = mbedtls_sha256_finish_ret(&ctx, out) == 0;
    }

    mbedtls_sha256_free(&ctx);
    return ok;
}

static u64 config_secret_nonce(const char *field, const char *plain, int version, bool bind_identity)
{
    unsigned char digest[32];
    u64 nonce = osGetTime() ^ ((u64)g_frame_counter << 32);
    if (config_secret_digest(field, nonce, "nonce", version, bind_identity, digest)) {
        for (int i = 0; i < 8; i++) {
            nonce = (nonce << 8) ^ digest[i] ^ (u64)digest[i + 8];
        }
    }
    for (size_t i = 0; plain && plain[i]; i++) {
        nonce = (nonce * 1099511628211ULL) ^ (unsigned char)plain[i];
    }
    return nonce ? nonce : 0x33444A656C6C7931ULL;
}

static bool config_secret_tag(const char *field, u64 nonce, const unsigned char *cipher, size_t cipher_len, int version, bool bind_identity, unsigned char tag[16])
{
    unsigned char auth_key[32];
    unsigned char digest[32];
    if (!config_secret_digest(field, nonce, "auth", version, bind_identity, auth_key)) {
        return false;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    bool ok = mbedtls_sha256_starts_ret(&ctx, 0) == 0;
    if (ok) {
        ok = mbedtls_sha256_update_ret(&ctx, auth_key, sizeof(auth_key)) == 0;
    }
    if (ok) {
        sha256_update_u64(&ctx, nonce);
        sha256_update_string(&ctx, field);
        ok = mbedtls_sha256_update_ret(&ctx, cipher, cipher_len) == 0;
    }
    if (ok) {
        ok = mbedtls_sha256_finish_ret(&ctx, digest) == 0;
    }
    mbedtls_sha256_free(&ctx);

    if (ok) {
        memcpy(tag, digest, 16);
    }
    memset(auth_key, 0, sizeof(auth_key));
    memset(digest, 0, sizeof(digest));
    return ok;
}

static bool config_secret_crypt(const char *field, u64 nonce, const unsigned char *in, unsigned char *out, size_t len, int version, bool bind_identity)
{
    unsigned char key[32];
    unsigned char iv_digest[32];
    unsigned char nonce_counter[16];
    unsigned char stream_block[16];
    size_t nc_off = 0;

    if (!config_secret_digest(field, nonce, "aes-key", version, bind_identity, key) ||
        !config_secret_digest(field, nonce, "aes-ctr", version, bind_identity, iv_digest)) {
        memset(key, 0, sizeof(key));
        memset(iv_digest, 0, sizeof(iv_digest));
        return false;
    }
    memcpy(nonce_counter, iv_digest, sizeof(nonce_counter));
    memset(stream_block, 0, sizeof(stream_block));

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    bool ok = mbedtls_aes_setkey_enc(&aes, key, 256) == 0 &&
              mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce_counter, stream_block, in, out) == 0;
    mbedtls_aes_free(&aes);

    memset(key, 0, sizeof(key));
    memset(iv_digest, 0, sizeof(iv_digest));
    memset(nonce_counter, 0, sizeof(nonce_counter));
    memset(stream_block, 0, sizeof(stream_block));
    return ok;
}

static bool config_encrypt_secret(const char *field, const char *plain, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return false;
    }
    out[0] = 0;
    if (!plain || !plain[0]) {
        return true;
    }

    size_t len = strlen(plain);
    if (len > 255 || outsz < 4 + 16 + 1 + 32 + 1 + len * 2 + 1) {
        return false;
    }

    unsigned char cipher[256];
    unsigned char tag[16];
    int version = 2;
    bool bind_identity = false;
    u64 nonce = config_secret_nonce(field, plain, version, bind_identity);
    if (!config_secret_crypt(field, nonce, (const unsigned char *)plain, cipher, len, version, bind_identity) ||
        !config_secret_tag(field, nonce, cipher, len, version, bind_identity, tag)) {
        memset(cipher, 0, sizeof(cipher));
        return false;
    }

    int prefix = snprintf(out, outsz, "v%d:%016" PRIX64 ":", version, (uint64_t)nonce);
    if (prefix < 0 || (size_t)prefix >= outsz) {
        memset(cipher, 0, sizeof(cipher));
        return false;
    }
    size_t w = (size_t)prefix;
    bool ok = hex_write_bytes(tag, sizeof(tag), out, outsz, &w) &&
              append_char(out, outsz, &w, ':') &&
              hex_write_bytes(cipher, len, out, outsz, &w);

    memset(cipher, 0, sizeof(cipher));
    memset(tag, 0, sizeof(tag));
    return ok;
}

static bool config_decrypt_secret(const char *field, const char *enc, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return false;
    }
    out[0] = 0;
    if (!enc || !enc[0]) {
        return true;
    }
    int version = 0;
    bool bind_identity = false;
    if (strncmp(enc, "v1:", 3) == 0) {
        version = 1;
        bind_identity = true;
    } else if (strncmp(enc, "v2:", 3) == 0) {
        version = 2;
        bind_identity = false;
    } else {
        return false;
    }

    const char *nonce_hex = enc + 3;
    const char *tag_hex = nonce_hex + 16;
    if (*tag_hex != ':') {
        return false;
    }
    tag_hex++;
    const char *cipher_hex = tag_hex + 32;
    if (*cipher_hex != ':') {
        return false;
    }
    cipher_hex++;

    size_t cipher_hex_len = strlen(cipher_hex);
    if ((cipher_hex_len & 1) != 0) {
        return false;
    }
    size_t len = cipher_hex_len / 2;
    if (len >= outsz || len > 255) {
        return false;
    }

    u64 nonce = 0;
    unsigned char expected_tag[16];
    unsigned char actual_tag[16];
    unsigned char cipher[256];
    unsigned char plain[256];
    if (!hex_read_u64_fixed(nonce_hex, 16, &nonce) ||
        !hex_read_bytes(tag_hex, expected_tag, sizeof(expected_tag)) ||
        !hex_read_bytes(cipher_hex, cipher, len) ||
        !config_secret_tag(field, nonce, cipher, len, version, bind_identity, actual_tag)) {
        return false;
    }

    bool ok = memcmp(expected_tag, actual_tag, sizeof(expected_tag)) == 0 &&
              config_secret_crypt(field, nonce, cipher, plain, len, version, bind_identity);
    if (ok) {
        memcpy(out, plain, len);
        out[len] = 0;
    }

    memset(expected_tag, 0, sizeof(expected_tag));
    memset(actual_tag, 0, sizeof(actual_tag));
    memset(cipher, 0, sizeof(cipher));
    memset(plain, 0, sizeof(plain));
    return ok;
}

static void save_config_secret(FILE *f, const char *key, const char *field, const char *value)
{
    char enc[512];
    if (value && value[0] && config_encrypt_secret(field, value, enc, sizeof(enc))) {
        fprintf(f, "%s_enc=%s\n", key, enc);
    } else {
        fprintf(f, "%s_enc=\n", key);
    }
}

static void ensure_defaults(void)
{
    if (!g_cfg.server[0]) {
        copy_safe(g_cfg.server, sizeof(g_cfg.server), "http://192.168.1.2:8096");
    }
    normalize_server_url(g_cfg.server);

    if (!g_cfg.device_id[0]) {
        snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "3dJelly-%08lX", (unsigned long)osGetTime());
    }
    if (!is_supported_quality(g_cfg.quality)) {
        g_cfg.quality = default_quality();
    }
    if (!is_supported_playback_mode(g_cfg.playback_mode)) {
        g_cfg.playback_mode = PLAYBACK_MODE_AUTO;
    }
    g_cfg.stream_buffer_kb = settings_buffer_normalized_kb(g_cfg.stream_buffer_kb);
    g_cfg.audio_sample_rate = settings_audio_sample_rate_normalized(g_cfg.audio_sample_rate);
    if (g_cfg.volume_percent < VOLUME_MIN_PERCENT || g_cfg.volume_percent > VOLUME_MAX_PERCENT) {
        g_cfg.volume_percent = VOLUME_DEFAULT_PERCENT;
    }
    g_cfg.language = language_normalized(g_cfg.language);
}

static void save_config(void)
{
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        return;
    }

    fprintf(f, "server=%s\n", g_cfg.server);
    fprintf(f, "username=%s\n", g_cfg.username);
    save_config_secret(f, "password", "password", g_cfg.password);
    save_config_secret(f, "token", "token", g_cfg.token);
    fprintf(f, "user_id=%s\n", g_cfg.user_id);
    fprintf(f, "device_id=%s\n", g_cfg.device_id);
    fprintf(f, "quality=%d\n", g_cfg.quality);
    fprintf(f, "playback_mode=%d\n", g_cfg.playback_mode);
    fprintf(f, "stream_buffer_kb=%d\n", g_cfg.stream_buffer_kb);
    fprintf(f, "audio_sample_rate=%d\n", g_cfg.audio_sample_rate);
    fprintf(f, "volume_percent=%d\n", g_cfg.volume_percent);
    fprintf(f, "language=%d\n", g_cfg.language);
    fclose(f);
}

static void load_config(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.playback_mode = PLAYBACK_MODE_AUTO;
    g_cfg.stream_buffer_kb = SETTINGS_BUFFER_KB_DEFAULT;
    g_cfg.audio_sample_rate = 0;
    g_cfg.volume_percent = -1;
    g_cfg.language = LANG_EN;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ensure_defaults();
        return;
    }

    char password_enc[512] = "";
    char token_enc[512] = "";
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq++ = 0;
        if (strcmp(line, "server") == 0) {
            copy_safe(g_cfg.server, sizeof(g_cfg.server), eq);
        } else if (strcmp(line, "username") == 0) {
            copy_safe(g_cfg.username, sizeof(g_cfg.username), eq);
        } else if (strcmp(line, "password") == 0) {
            copy_safe(g_cfg.password, sizeof(g_cfg.password), eq);
        } else if (strcmp(line, "password_enc") == 0) {
            copy_safe(password_enc, sizeof(password_enc), eq);
        } else if (strcmp(line, "token") == 0) {
            copy_safe(g_cfg.token, sizeof(g_cfg.token), eq);
        } else if (strcmp(line, "token_enc") == 0) {
            copy_safe(token_enc, sizeof(token_enc), eq);
        } else if (strcmp(line, "user_id") == 0) {
            copy_safe(g_cfg.user_id, sizeof(g_cfg.user_id), eq);
        } else if (strcmp(line, "device_id") == 0) {
            copy_safe(g_cfg.device_id, sizeof(g_cfg.device_id), eq);
        } else if (strcmp(line, "quality") == 0) {
            g_cfg.quality = atoi(eq);
        } else if (strcmp(line, "playback_mode") == 0) {
            g_cfg.playback_mode = atoi(eq);
        } else if (strcmp(line, "stream_buffer_kb") == 0) {
            g_cfg.stream_buffer_kb = atoi(eq);
        } else if (strcmp(line, "audio_sample_rate") == 0) {
            g_cfg.audio_sample_rate = atoi(eq);
        } else if (strcmp(line, "volume_percent") == 0) {
            g_cfg.volume_percent = atoi(eq);
        } else if (strcmp(line, "language") == 0) {
            g_cfg.language = atoi(eq);
        }
    }
    fclose(f);
    ensure_defaults();

    char decrypted[192];
    bool decrypt_failed = false;
    if (password_enc[0] && config_decrypt_secret("password", password_enc, decrypted, sizeof(g_cfg.password))) {
        copy_safe(g_cfg.password, sizeof(g_cfg.password), decrypted);
    } else if (password_enc[0]) {
        decrypt_failed = true;
    }
    if (token_enc[0] && config_decrypt_secret("token", token_enc, decrypted, sizeof(g_cfg.token))) {
        copy_safe(g_cfg.token, sizeof(g_cfg.token), decrypted);
    } else if (token_enc[0]) {
        decrypt_failed = true;
        g_cfg.token[0] = 0;
        g_cfg.user_id[0] = 0;
    }
    memset(decrypted, 0, sizeof(decrypted));
    if (decrypt_failed) {
        set_status("%s", tr(TR_STATUS_CREDENTIAL_DECRYPT_FAIL));
    } else if (!g_cfg.token[0] || !g_cfg.user_id[0]) {
        set_status("%s", tr(TR_STATUS_CONFIGURE_SERVER));
    }
}

static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t w = 0;
    if (!outsz) {
        return;
    }
    for (size_t i = 0; in && in[i] && w + 2 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c == '\n') {
            out[w++] = '\\';
            out[w++] = 'n';
        } else if (c == '\r') {
            out[w++] = '\\';
            out[w++] = 'r';
        } else if (c == '\t') {
            out[w++] = '\\';
            out[w++] = 't';
        } else if (c >= 0x20) {
            out[w++] = (char)c;
        }
    }
    out[w] = 0;
}

static void url_encode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    if (!outsz) {
        return;
    }
    for (size_t i = 0; in && in[i] && w + 4 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[w++] = (char)c;
        } else {
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 15];
        }
    }
    out[w] = 0;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *find_key_range(const char *start, const char *end, const char *key)
{
    size_t klen = strlen(key);
    int depth = 0;
    bool in_str = false;
    bool esc = false;

    for (const char *p = start; p + klen + 2 < end; p++) {
        char c = *p;
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }

        if (c == '{' || c == '[') {
            depth++;
            continue;
        }
        if (c == '}' || c == ']') {
            depth--;
            continue;
        }

        if (c != '"') {
            continue;
        }

        if (depth == 1 && (size_t)(end - p) > klen + 2 && strncmp(p + 1, key, klen) == 0 && p[klen + 1] == '"') {
            const char *q = skip_ws(p + klen + 2, end);
            if (q < end && *q == ':') {
                return skip_ws(q + 1, end);
            }
        }

        in_str = true;
    }
    return NULL;
}

static bool json_get_string_range(const char *start, const char *end, const char *key, char *out, size_t outsz)
{
    const char *p = find_key_range(start, end, key);
    if (!p || p >= end || *p != '"') {
        if (outsz) {
            out[0] = 0;
        }
        return false;
    }

    p++;
    size_t w = 0;
    if (outsz) {
        out[0] = 0;
    }
    while (p < end && *p != '"' && w + 1 < outsz) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case '"':
            case '\\':
            case '/':
                append_char(out, outsz, &w, *p);
                p++;
                continue;
            case 'b':
                append_char(out, outsz, &w, '\b');
                p++;
                continue;
            case 'f':
                append_char(out, outsz, &w, '\f');
                p++;
                continue;
            case 'n':
                append_char(out, outsz, &w, '\n');
                p++;
                continue;
            case 'r':
                append_char(out, outsz, &w, '\r');
                p++;
                continue;
            case 't':
                append_char(out, outsz, &w, '\t');
                p++;
                continue;
            case 'u':
            {
                u32 cp = 0;
                if (!read_json_hex4(p + 1, end, &cp)) {
                    append_char(out, outsz, &w, '?');
                    p++;
                    continue;
                }
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                    u32 lo = 0;
                    if (read_json_hex4(p + 2, end, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        p += 6;
                    }
                }
                if (!append_display_codepoint(out, outsz, &w, cp)) {
                    append_char(out, outsz, &w, '?');
                }
                continue;
            }
            default:
                append_char(out, outsz, &w, *p);
                p++;
                continue;
            }
        } else {
            u32 cp = 0;
            size_t len = read_utf8_codepoint(p, end, &cp);
            if (len > 0) {
                if (!append_display_codepoint(out, outsz, &w, cp)) {
                    append_char(out, outsz, &w, '?');
                }
                p += len;
            } else {
                append_char(out, outsz, &w, *p);
                p++;
            }
            continue;
        }
    }
    if (outsz) {
        out[w] = 0;
    }
    return true;
}

static bool json_get_bool_range(const char *start, const char *end, const char *key, bool *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) {
        return false;
    }
    if (p + 4 <= end && strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (p + 5 <= end && strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool json_get_int_range(const char *start, const char *end, const char *key, int *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) {
        return false;
    }
    *out = atoi(p);
    return true;
}

static bool json_get_ull_range(const char *start, const char *end, const char *key, unsigned long long *out)
{
    const char *p = find_key_range(start, end, key);
    if (!p) {
        return false;
    }
    *out = strtoull(p, NULL, 10);
    return true;
}

static bool json_object_range_after(const char *p, const char *end, const char **obj_start, const char **obj_end)
{
    p = skip_ws(p, end);
    if (p >= end || *p != '{') {
        return false;
    }

    int depth = 0;
    bool in_str = false;
    bool esc = false;
    const char *q = p;
    for (; q < end; q++) {
        char c = *q;
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                *obj_start = p;
                *obj_end = q + 1;
                return true;
            }
        }
    }
    return false;
}

static bool json_get_object_range(const char *start, const char *end, const char *key, const char **obj_start, const char **obj_end)
{
    const char *p = find_key_range(start, end, key);
    if (!p) {
        return false;
    }
    return json_object_range_after(p, end, obj_start, obj_end);
}
