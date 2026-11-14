#include "shell.h"
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int io_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == ENOTSOCK) w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

int io_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

int frame_send(int fd, int type, const void *p, uint32_t n)
{
    unsigned char h[5] = { (unsigned char)type, (unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n };
    if (n > FRAME_MAX) return -1;
    unsigned char *b = malloc(5 + (size_t)n);      /* one write: frames never interleave */
    if (!b) return -1;
    memcpy(b, h, 5);
    if (n) memcpy(b + 5, p, n);
    int rc = io_all(fd, b, 5 + (size_t)n);
    free(b);
    return rc;
}

int frame_recv(int fd, int *type, unsigned char *buf, uint32_t cap, uint32_t *n)
{
    unsigned char h[5];
    if (io_full(fd, h, 5) < 0) return -1;
    uint32_t len = (uint32_t)h[1] << 24 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 8 | h[4];
    if (len > cap || len > FRAME_MAX) return -1;
    if (len && io_full(fd, buf, len) < 0) return -1;
    *type = h[0];
    *n = len;
    return 0;
}
