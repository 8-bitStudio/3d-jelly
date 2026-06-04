typedef enum {
    REMOTE_CMD_NONE,
    REMOTE_CMD_PAUSE,
    REMOTE_CMD_UNPAUSE,
    REMOTE_CMD_TOGGLE_PAUSE,
    REMOTE_CMD_STOP,
    REMOTE_CMD_SEEK,
    REMOTE_CMD_MESSAGE,
    REMOTE_CMD_MUTE,
    REMOTE_CMD_UNMUTE,
    REMOTE_CMD_TOGGLE_MUTE,
    REMOTE_CMD_VOLUME_UP,
    REMOTE_CMD_VOLUME_DOWN,
    REMOTE_CMD_SET_VOLUME
} RemoteCommandType;

typedef struct {
    RemoteCommandType type;
    u64 seek_ticks;
    int volume_percent;
    int timeout_ms;
    char header[64];
    char text[192];
} RemoteCommand;

typedef struct {
    bool soc_ready;
    bool connected;
    bool tls_unsupported;
    int sock;
    u32 *soc_buffer;
    u8 recv_buf[WEBSOCKET_RECV_CAP];
    size_t recv_size;
    char message[WEBSOCKET_MESSAGE_CAP];
} RemoteControlSocket;

static RemoteControlSocket g_remote_ws;

static void remote_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_remote_status, sizeof(g_remote_status), fmt, ap);
    va_end(ap);
}

static int remote_clamp_int(int value, int lo, int hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static void remote_command_clear(RemoteCommand *cmd)
{
    if (cmd) {
        memset(cmd, 0, sizeof(*cmd));
        cmd->type = REMOTE_CMD_NONE;
    }
}

static void remote_show_message(const char *header, const char *text, int timeout_ms)
{
    copy_safe(g_remote_message_header, sizeof(g_remote_message_header), header && header[0] ? header : "Jellyfin");
    copy_safe(g_remote_message_text, sizeof(g_remote_message_text), text && text[0] ? text : "");
    timeout_ms = remote_clamp_int(timeout_ms, 1000, 15000);
    g_remote_message_until_ms = osGetTime() + (u64)timeout_ms;
}

static bool remote_message_osd_visible(void)
{
    return g_remote_message_until_ms && osGetTime() < g_remote_message_until_ms && g_remote_message_text[0];
}

static bool remote_parse_server_url(char *host, size_t hostsz, char *port, size_t portsz, char *base_path, size_t pathsz)
{
    const char *server = g_cfg.server;
    if (!starts_with_http(server)) {
        remote_set_status("REMOTE BAD SERVER URL");
        return false;
    }
    if (strncmp(server, "https://", 8) == 0) {
        g_remote_ws.tls_unsupported = true;
        remote_set_status("REMOTE HTTPS UNSUPPORTED");
        return false;
    }
    if (strncmp(server, "http://", 7) != 0) {
        remote_set_status("REMOTE BAD SERVER URL");
        return false;
    }
    g_remote_ws.tls_unsupported = false;

    const char *p = server + 7;
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : server + strlen(server);
    const char *colon = NULL;
    for (const char *q = p; q < host_end; q++) {
        if (*q == ':') {
            colon = q;
            break;
        }
    }

    const char *name_end = colon ? colon : host_end;
    size_t host_len = (size_t)(name_end - p);
    if (host_len == 0 || host_len >= hostsz) {
        remote_set_status("REMOTE BAD HOST");
        return false;
    }
    memcpy(host, p, host_len);
    host[host_len] = 0;

    if (colon) {
        size_t port_len = (size_t)(host_end - colon - 1);
        if (port_len == 0 || port_len >= portsz) {
            remote_set_status("REMOTE BAD PORT");
            return false;
        }
        memcpy(port, colon + 1, port_len);
        port[port_len] = 0;
    } else {
        copy_safe(port, portsz, "80");
    }

    if (slash && slash[0]) {
        copy_safe(base_path, pathsz, slash);
        size_t len = strlen(base_path);
        while (len > 1 && base_path[len - 1] == '/') {
            base_path[--len] = 0;
        }
    } else {
        copy_safe(base_path, pathsz, "");
    }
    return true;
}

static bool remote_socket_wait(int sock, bool write, int timeout_ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ret = select(sock + 1, write ? NULL : &fds, write ? &fds : NULL, NULL, &tv);
    return ret > 0 && FD_ISSET(sock, &fds);
}

static bool remote_send_all_timeout(int sock, const u8 *data, size_t size, int timeout_ms);

static bool remote_send_all(int sock, const u8 *data, size_t size)
{
    return remote_send_all_timeout(sock, data, size, 1200);
}

static bool remote_send_all_timeout(int sock, const u8 *data, size_t size, int timeout_ms)
{
    while (size > 0) {
        if (!remote_socket_wait(sock, true, timeout_ms)) {
            return false;
        }
        int sent = send(sock, data, size, 0);
        if (sent <= 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        data += sent;
        size -= (size_t)sent;
    }
    return true;
}

static void remote_close_socket(void)
{
    if (g_remote_ws.connected && g_remote_ws.sock >= 0) {
        close(g_remote_ws.sock);
    }
    g_remote_ws.connected = false;
    g_remote_ws.sock = -1;
    g_remote_ws.recv_size = 0;
}

static void remote_shutdown(void)
{
    remote_close_socket();
    if (g_remote_ws.soc_ready) {
        socExit();
        g_remote_ws.soc_ready = false;
    }
    if (g_remote_ws.soc_buffer) {
        free(g_remote_ws.soc_buffer);
        g_remote_ws.soc_buffer = NULL;
    }
}

static bool remote_socket_init(void)
{
    if (g_remote_ws.soc_ready) {
        return true;
    }

    g_remote_ws.sock = -1;
    g_remote_ws.soc_buffer = (u32 *)memalign(0x1000, WEBSOCKET_SOC_BUFFER_SIZE);
    if (!g_remote_ws.soc_buffer) {
        remote_set_status("REMOTE SOC MEM FAIL");
        return false;
    }

    Result ret = socInit(g_remote_ws.soc_buffer, WEBSOCKET_SOC_BUFFER_SIZE);
    if (R_FAILED(ret)) {
        g_remote_last_result = ret;
        free(g_remote_ws.soc_buffer);
        g_remote_ws.soc_buffer = NULL;
        remote_set_status("REMOTE SOC FAIL 0x%08lX", (unsigned long)ret);
        return false;
    }
    g_remote_ws.soc_ready = true;
    remote_set_status("REMOTE SOC READY");
    return true;
}

static int remote_connect_tcp(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0 || !res) {
        g_remote_last_errno = errno;
        remote_set_status("REMOTE DNS FAIL %d", gai);
        return -1;
    }

    int sock = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            g_remote_last_errno = errno;
            continue;
        }

        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }

        int ret = connect(sock, ai->ai_addr, ai->ai_addrlen);
        int connect_errno = errno;
        if (ret == 0 || errno == EINPROGRESS) {
            if (remote_socket_wait(sock, true, timeout_ms)) {
                int err = 0;
                socklen_t err_len = sizeof(err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0) {
                    if (err == 0) {
                        break;
                    }
                    g_remote_last_errno = err;
                } else {
                    g_remote_last_errno = errno;
                }
            } else {
                g_remote_last_errno = ETIMEDOUT;
            }
        } else {
            g_remote_last_errno = connect_errno;
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);
    if (sock < 0) {
        remote_set_status("REMOTE CONNECT FAIL %d", g_remote_last_errno);
    }
    return sock;
}

static bool remote_websocket_send_frame(u8 opcode, const u8 *payload, size_t payload_size)
{
    if (!g_remote_ws.connected || g_remote_ws.sock < 0 || payload_size > 125) {
        return false;
    }

    u8 frame[2 + 4 + 125];
    frame[0] = 0x80 | (opcode & 0x0F);
    frame[1] = 0x80 | (u8)payload_size;
    u32 mask = (u32)osGetTime() ^ 0x33444A59U;
    frame[2] = (u8)(mask & 0xFF);
    frame[3] = (u8)((mask >> 8) & 0xFF);
    frame[4] = (u8)((mask >> 16) & 0xFF);
    frame[5] = (u8)((mask >> 24) & 0xFF);
    for (size_t i = 0; i < payload_size; i++) {
        frame[6 + i] = payload[i] ^ frame[2 + (i & 3)];
    }
    return remote_send_all(g_remote_ws.sock, frame, 6 + payload_size);
}

static bool remote_websocket_send_text(const char *text)
{
    return remote_websocket_send_frame(0x1, (const u8 *)text, strlen(text));
}

static bool remote_websocket_handshake(int sock, const char *host, const char *port, const char *endpoint)
{
    char auth[384];
    char request[4096];
    auth_header(auth, sizeof(auth), true);
    int request_len = snprintf(request, sizeof(request),
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s:%s\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                               "Sec-WebSocket-Version: 13\r\n"
                               "User-Agent: 3dJelly/%s Nintendo 3DS\r\n"
                               "Authorization: %s\r\n"
                               "X-Emby-Authorization: %s\r\n"
                               "X-Emby-Token: %s\r\n"
                               "X-MediaBrowser-Token: %s\r\n"
                               "\r\n",
                               endpoint, host, port, APP_VERSION, auth, auth, g_cfg.token, g_cfg.token);
    if (request_len <= 0 || (size_t)request_len >= sizeof(request)) {
        remote_set_status("REMOTE REQUEST TOO LONG");
        return false;
    }

    if (!remote_send_all(sock, (const u8 *)request, (size_t)request_len)) {
        remote_set_status("REMOTE SEND FAIL %d", errno);
        return false;
    }

    char response[1024];
    size_t used = 0;
    u64 start_ms = osGetTime();
    while (used + 1 < sizeof(response) && osGetTime() - start_ms < 2500) {
        if (!remote_socket_wait(sock, false, 250)) {
            continue;
        }
        int got = recv(sock, response + used, sizeof(response) - used - 1, 0);
        if (got <= 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        used += (size_t)got;
        response[used] = 0;
        if (strstr(response, "\r\n\r\n")) {
            int code = 0;
            sscanf(response, "HTTP/%*s %d", &code);
            g_remote_last_http_status = code;
            if (code == 101) {
                return true;
            }
            remote_set_status("REMOTE HTTP %d", code);
            return false;
        }
    }
    remote_set_status("REMOTE HANDSHAKE TIMEOUT");
    return false;
}

static bool remote_http_post_json_quick(const char *path, const char *body, int timeout_ms)
{
    if (!path || !body || !g_cfg.token[0]) {
        remote_set_status("REMOTE POST MISSING TOKEN");
        return false;
    }
    if (!remote_socket_init()) {
        return false;
    }

    char host[192];
    char port[16];
    char base_path[192];
    if (!remote_parse_server_url(host, sizeof(host), port, sizeof(port), base_path, sizeof(base_path))) {
        return false;
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s%s", base_path, path[0] == '/' ? "" : "/", path);

    char auth[384];
    auth_header(auth, sizeof(auth), true);

    char request[3072];
    int len = snprintf(request, sizeof(request),
                       "POST %s HTTP/1.1\r\n"
                       "Host: %s:%s\r\n"
                       "User-Agent: 3dJelly/%s Nintendo 3DS\r\n"
                       "Authorization: %s\r\n"
                       "X-Emby-Authorization: %s\r\n"
                       "X-Emby-Token: %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %lu\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%s",
                       full_path, host, port, APP_VERSION, auth, auth, g_cfg.token,
                       (unsigned long)strlen(body), body);
    if (len <= 0 || (size_t)len >= sizeof(request)) {
        return false;
    }

    int sock = remote_connect_tcp(host, port, timeout_ms);
    if (sock < 0) {
        return false;
    }
    bool ok = remote_send_all_timeout(sock, (const u8 *)request, (size_t)len, timeout_ms);
    close(sock);
    return ok;
}

static bool remote_control_start(void)
{
    if (g_remote_ws.connected) {
        remote_set_status("REMOTE WS OK");
        return true;
    }
    if (!g_cfg.token[0] || !g_cfg.device_id[0]) {
        return false;
    }
    if (!remote_socket_init()) {
        return false;
    }

    char host[192];
    char port[16];
    char base_path[192];
    if (!remote_parse_server_url(host, sizeof(host), port, sizeof(port), base_path, sizeof(base_path))) {
        return false;
    }

    char token[256];
    char device[160];
    char endpoint[1536];
    url_encode(g_cfg.token, token, sizeof(token));
    url_encode(g_cfg.device_id, device, sizeof(device));
    int endpoint_len = snprintf(endpoint, sizeof(endpoint),
                                "%s/socket?api_key=%s&ApiKey=%s&deviceId=%s",
                                base_path, token, token, device);
    if (endpoint_len <= 0 || (size_t)endpoint_len >= sizeof(endpoint)) {
        remote_set_status("REMOTE URL TOO LONG");
        return false;
    }

    int sock = remote_connect_tcp(host, port, 1800);
    if (sock < 0) {
        return false;
    }
    if (!remote_websocket_handshake(sock, host, port, endpoint)) {
        close(sock);
        return false;
    }

    g_remote_ws.sock = sock;
    g_remote_ws.connected = true;
    g_remote_ws.recv_size = 0;
    remote_set_status("REMOTE WS OK");
    return true;
}

static void remote_control_stop(void)
{
    if (g_remote_ws.connected) {
        remote_websocket_send_frame(0x8, NULL, 0);
    }
    remote_shutdown();
}

static bool remote_parse_playstate(const char *start, const char *end, RemoteCommand *cmd)
{
    char command[32];
    if (!json_get_string_range(start, end, "Command", command, sizeof(command))) {
        return false;
    }

    if (strcmp(command, "Pause") == 0) {
        cmd->type = REMOTE_CMD_PAUSE;
    } else if (strcmp(command, "Unpause") == 0 || strcmp(command, "Play") == 0) {
        cmd->type = REMOTE_CMD_UNPAUSE;
    } else if (strcmp(command, "PlayPause") == 0) {
        cmd->type = REMOTE_CMD_TOGGLE_PAUSE;
    } else if (strcmp(command, "Stop") == 0) {
        cmd->type = REMOTE_CMD_STOP;
    } else if (strcmp(command, "Seek") == 0) {
        unsigned long long ticks = 0;
        if (!json_get_ull_range(start, end, "SeekPositionTicks", &ticks)) {
            return false;
        }
        cmd->seek_ticks = clamp_media_ticks((u64)ticks);
        cmd->type = REMOTE_CMD_SEEK;
    } else {
        return false;
    }
    return true;
}

static bool remote_get_arg_int(const char *start, const char *end, const char *key, int *out)
{
    if (json_get_int_range(start, end, key, out)) {
        return true;
    }
    char text[24];
    if (json_get_string_range(start, end, key, text, sizeof(text))) {
        *out = atoi(text);
        return true;
    }
    return false;
}

static bool remote_parse_message_command(const char *start, const char *end, RemoteCommand *cmd)
{
    char header[64];
    char text[192];
    int timeout = REMOTE_MESSAGE_OSD_MS;
    json_get_string_range(start, end, "Header", header, sizeof(header));
    if (!json_get_string_range(start, end, "Text", text, sizeof(text)) &&
        !json_get_string_range(start, end, "Message", text, sizeof(text))) {
        return false;
    }
    json_get_int_range(start, end, "TimeoutMs", &timeout);

    cmd->type = REMOTE_CMD_MESSAGE;
    cmd->timeout_ms = timeout;
    copy_safe(cmd->header, sizeof(cmd->header), header[0] ? header : "Jellyfin");
    copy_safe(cmd->text, sizeof(cmd->text), text);
    return true;
}

static bool remote_parse_general_command(const char *start, const char *end, RemoteCommand *cmd)
{
    char name[48];
    if (!json_get_string_range(start, end, "Name", name, sizeof(name))) {
        return false;
    }

    const char *args = NULL;
    const char *args_end = NULL;
    json_get_object_range(start, end, "Arguments", &args, &args_end);

    if (strcmp(name, "DisplayMessage") == 0) {
        if (args && remote_parse_message_command(args, args_end, cmd)) {
            return true;
        }
        return remote_parse_message_command(start, end, cmd);
    }
    if (strcmp(name, "PlayState") == 0 && args) {
        return remote_parse_playstate(args, args_end, cmd);
    }
    if (strcmp(name, "Mute") == 0) {
        cmd->type = REMOTE_CMD_MUTE;
    } else if (strcmp(name, "Unmute") == 0) {
        cmd->type = REMOTE_CMD_UNMUTE;
    } else if (strcmp(name, "ToggleMute") == 0) {
        cmd->type = REMOTE_CMD_TOGGLE_MUTE;
    } else if (strcmp(name, "VolumeUp") == 0) {
        cmd->type = REMOTE_CMD_VOLUME_UP;
    } else if (strcmp(name, "VolumeDown") == 0) {
        cmd->type = REMOTE_CMD_VOLUME_DOWN;
    } else if (strcmp(name, "SetVolume") == 0 && args) {
        int volume = 0;
        if (!remote_get_arg_int(args, args_end, "Volume", &volume) &&
            !remote_get_arg_int(args, args_end, "VolumeLevel", &volume)) {
            return false;
        }
        cmd->volume_percent = remote_clamp_int(volume, VOLUME_MIN_PERCENT, VOLUME_MAX_PERCENT);
        cmd->type = REMOTE_CMD_SET_VOLUME;
    } else {
        return false;
    }
    return true;
}

static bool remote_parse_websocket_message(const char *message, RemoteCommand *cmd)
{
    remote_command_clear(cmd);
    const char *end = message + strlen(message);
    char type[48];
    if (!json_get_string_range(message, end, "MessageType", type, sizeof(type))) {
        return false;
    }

    if (strcmp(type, "ForceKeepAlive") == 0 || strcmp(type, "KeepAlive") == 0) {
        remote_websocket_send_text("{\"MessageType\":\"KeepAlive\"}");
        return false;
    }

    const char *data = NULL;
    const char *data_end = NULL;
    if (!json_get_object_range(message, end, "Data", &data, &data_end)) {
        data = message;
        data_end = end;
    }

    if (strcmp(type, "Playstate") == 0 || strcmp(type, "PlayState") == 0) {
        return remote_parse_playstate(data, data_end, cmd);
    }
    if (strcmp(type, "GeneralCommand") == 0) {
        return remote_parse_general_command(data, data_end, cmd);
    }
    if (strcmp(type, "MessageCommand") == 0 || strcmp(type, "Message") == 0) {
        return remote_parse_message_command(data, data_end, cmd);
    }
    return false;
}

static void remote_consume_recv(size_t bytes)
{
    if (bytes >= g_remote_ws.recv_size) {
        g_remote_ws.recv_size = 0;
        return;
    }
    memmove(g_remote_ws.recv_buf, g_remote_ws.recv_buf + bytes, g_remote_ws.recv_size - bytes);
    g_remote_ws.recv_size -= bytes;
}

static bool remote_process_buffer(RemoteCommand *cmd)
{
    while (g_remote_ws.recv_size >= 2) {
        const u8 *buf = g_remote_ws.recv_buf;
        u8 opcode = buf[0] & 0x0F;
        bool masked = (buf[1] & 0x80) != 0;
        u64 len = buf[1] & 0x7F;
        size_t header = 2;
        if (len == 126) {
            if (g_remote_ws.recv_size < 4) {
                return false;
            }
            len = ((u64)buf[2] << 8) | buf[3];
            header = 4;
        } else if (len == 127) {
            if (g_remote_ws.recv_size < 10) {
                return false;
            }
            len = 0;
            for (int i = 0; i < 8; i++) {
                len = (len << 8) | buf[2 + i];
            }
            header = 10;
        }

        u8 mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (g_remote_ws.recv_size < header + 4) {
                return false;
            }
            memcpy(mask, buf + header, 4);
            header += 4;
        }

        if (len > WEBSOCKET_MESSAGE_CAP - 1) {
            remote_close_socket();
            return false;
        }
        if (g_remote_ws.recv_size < header + (size_t)len) {
            return false;
        }

        const u8 *payload = buf + header;
        if (opcode == 0x8) {
            remote_set_status("REMOTE WS CLOSED");
            remote_close_socket();
            return false;
        }
        if (opcode == 0x9) {
            remote_websocket_send_frame(0xA, payload, (size_t)len <= 125 ? (size_t)len : 0);
            remote_consume_recv(header + (size_t)len);
            continue;
        }
        if (opcode == 0x1) {
            for (size_t i = 0; i < (size_t)len; i++) {
                g_remote_ws.message[i] = (char)(payload[i] ^ mask[i & 3]);
            }
            g_remote_ws.message[len] = 0;
            remote_consume_recv(header + (size_t)len);
            if (remote_parse_websocket_message(g_remote_ws.message, cmd)) {
                return true;
            }
            continue;
        }

        remote_consume_recv(header + (size_t)len);
    }
    return false;
}

static bool remote_control_poll(RemoteCommand *cmd)
{
    remote_command_clear(cmd);
    if (!g_remote_ws.connected || g_remote_ws.sock < 0) {
        return false;
    }

    if (remote_process_buffer(cmd)) {
        return true;
    }

    while (remote_socket_wait(g_remote_ws.sock, false, 0)) {
        if (g_remote_ws.recv_size >= sizeof(g_remote_ws.recv_buf)) {
            remote_close_socket();
            return false;
        }
        int got = recv(g_remote_ws.sock,
                       g_remote_ws.recv_buf + g_remote_ws.recv_size,
                       sizeof(g_remote_ws.recv_buf) - g_remote_ws.recv_size,
                       0);
        if (got <= 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                break;
            }
            g_remote_last_errno = errno;
            remote_set_status("REMOTE RECV FAIL %d", errno);
            remote_close_socket();
            return false;
        }
        g_remote_ws.recv_size += (size_t)got;
        if (remote_process_buffer(cmd)) {
            return true;
        }
    }
    return false;
}
