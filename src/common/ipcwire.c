#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ipcwire.h"

int ipc_connect(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int ipc_readline(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return 0;
}

int ipc_send(int fd, const char *json)
{
    size_t len = strlen(json);
    /* json + '\n' */
    char *buf = malloc(len + 2);
    if (!buf) return -1;
    memcpy(buf, json, len);
    buf[len]     = '\n';
    buf[len + 1] = '\0';
    size_t off = 0;
    while (off < len + 1) {
        ssize_t w = write(fd, buf + off, len + 1 - off);
        if (w <= 0) { free(buf); return -1; }
        off += (size_t)w;
    }
    free(buf);
    return 0;
}

void ipc_json_escape(const char *src, char *dst, size_t cap)
{
    size_t i = 0, o = 0;
    while (src[i] && o + 6 < cap) {
        unsigned char c = (unsigned char)src[i++];
        if      (c == '"')  { dst[o++] = '\\'; dst[o++] = '"';  }
        else if (c == '\\') { dst[o++] = '\\'; dst[o++] = '\\'; }
        else if (c < 0x20)  { o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c); }
        else                 { dst[o++] = (char)c; }
    }
    dst[o] = '\0';
}

char *ipc_get_str(const char *json, const char *key)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return NULL;
    p++;
    char buf[4096];
    size_t n = 0;
    while (*p && n < sizeof buf - 1) {
        if (*p == '\\' && p[1] == '"') { buf[n++] = '"'; p += 2; }
        else if (*p == '\\' && p[1] == '\\') { buf[n++] = '\\'; p += 2; }
        else if (*p == '"') break;
        else buf[n++] = *p++;
    }
    buf[n] = '\0';
    return strdup(buf);
}

long long ipc_get_int(const char *json, const char *key, long long def)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return def;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '-' || (*p >= '0' && *p <= '9'))
        return strtoll(p, NULL, 10);
    return def;
}
