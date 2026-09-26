/* Use an explicit client reply port: binding port zero fails with EINVAL on
   the 3DS SOC service. Jellyfin replies to the query's source port. */
static int setup_bind_scan_socket(int fd, unsigned int seed)
{
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    for (unsigned int attempt = 0; attempt < 32; attempt++) {
        local.sin_port = htons(49152u + ((seed + attempt) & 0x3FFFu));
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) == 0)
            return 0;
        if (errno != EADDRINUSE)
            return -1;
    }
    return -1;
}
