/* Host regression test: gcc -Wall -Wextra -Werror tests/setup_socket_test.c -o build/setup_socket_test */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

static int mode, calls;
static unsigned int expected_seed;

static int test_bind(int fd, const struct sockaddr *addr, socklen_t size)
{
    const struct sockaddr_in *local = (const struct sockaddr_in *)addr;
    assert(size == sizeof(*local));
    assert(local->sin_family == AF_INET);
    assert(local->sin_addr.s_addr == INADDR_ANY);
    assert(ntohs(local->sin_port) >= 49152);
    if (!mode) return bind(fd, addr, size);
    assert(ntohs(local->sin_port) == 49152u + ((expected_seed + calls) & 0x3FFFu));
    calls++;
    if (mode == 1 && calls == 3) return 0;
    errno = mode == 3 ? EINVAL : EADDRINUSE;
    return -1;
}

#define bind test_bind
#include "../source/parts/setup_socket.inc"
#undef bind

static void check_reply(int server, int client)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    assert(getsockname(server, (struct sockaddr *)&address, &size) == 0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const char probe[] = "who is JellyfinServer?";
    assert(sendto(client, probe, sizeof(probe), 0,
                  (struct sockaddr *)&address, size) == sizeof(probe));
    char buf[128];
    struct timeval timeout = {2, 0};
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(server, &ready);
    assert(select(server + 1, &ready, NULL, NULL, &timeout) == 1);
    assert(recvfrom(server, buf, sizeof(buf), 0,
                    (struct sockaddr *)&address, &size) == sizeof(probe));
    assert(strcmp(buf, probe) == 0);
    const char reply[] = "{\"Id\":\"test\",\"Address\":\"http://127.0.0.1:8096\"}";
    assert(sendto(server, reply, sizeof(reply), 0,
                  (struct sockaddr *)&address, size) == sizeof(reply));
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    FD_ZERO(&ready);
    FD_SET(client, &ready);
    assert(select(client + 1, &ready, NULL, NULL, &timeout) == 1);
    assert(recv(client, buf, sizeof(buf), 0) == sizeof(reply));
    assert(strcmp(buf, reply) == 0);
}

int main(void)
{
    /* Port collisions retry, including wrapping at the end of the range. */
    mode = 1;
    expected_seed = 16383;
    assert(setup_bind_scan_socket(-1, expected_seed) == 0);
    assert(calls == 3);
    /* Exhaustion is bounded and preserves the failure for the UI. */
    mode = 2;
    calls = 0;
    assert(setup_bind_scan_socket(-1, expected_seed) == -1);
    assert(calls == 32 && errno == EADDRINUSE);
    /* Other failures surface immediately. */
    mode = 3;
    calls = 0;
    assert(setup_bind_scan_socket(-1, expected_seed) == -1);
    assert(calls == 1 && errno == EINVAL);
    /* Real UDP sockets: an occupied first port, response, close and retry. */
    mode = 0;
    int server = socket(AF_INET, SOCK_DGRAM, 0);
    assert(server >= 0 && setup_bind_scan_socket(server, 1234) == 0);
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    assert(getsockname(server, (struct sockaddr *)&address, &size) == 0);
    unsigned int seed = ntohs(address.sin_port) - 49152u;
    for (int scan = 0; scan < 2; scan++) {
        int client = socket(AF_INET, SOCK_DGRAM, 0);
        assert(client >= 0 && setup_bind_scan_socket(client, seed) == 0);
        check_reply(server, client);
        close(client);
    }
    close(server);
    puts("PASS: explicit reply port, collision retry, wraparound, exhaustion, fatal error, UDP reply and repeated scans");
    return 0;
}
