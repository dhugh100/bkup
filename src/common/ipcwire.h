#ifndef BK_IPCWIRE_H
#define BK_IPCWIRE_H

#include <stddef.h>

/* The newline-delimited JSON wire shared by bkupd and its clients: one request
   object per connection, then a stream of event objects back until "done" or
   "error". The parser is deliberately flat -- it finds "key": anywhere in the
   line and reads the scalar that follows -- which is all these messages need.

   This lives in common/ so the daemon and the CLI client link the same copy;
   bin/bkup-gui still carries its own equivalents because it deliberately links
   nothing but GTK4 (see the Makefile). */

/* One root daemon serves every user, so the socket is a fixed shared path. It
   is world-connectable; the daemon authorizes each peer via SO_PEERCRED and
   serves only that user's section. */
#define BKUPD_SOCK_PATH "/run/bkupd.sock"

/* Connect to a daemon listening on sock_path. Returns a socket fd, or -1 if
   nothing is listening (which is how a client tells "no daemon" from a real
   failure). */
int ipc_connect(const char *sock_path);

/* Read one '\n'-terminated line from fd into buf (NUL-terminated, newline
   stripped).  Returns 0 on success, -1 on EOF or error. */
int ipc_readline(int fd, char *buf, size_t cap);

/* Write json + '\n' to fd.  Returns 0 on success, -1 on error. */
int ipc_send(int fd, const char *json);

/* Extract a string field from a flat JSON object.
   Returns a malloc'd copy, or NULL if the key is absent.  Caller frees. */
char *ipc_get_str(const char *json, const char *key);

/* Extract an integer field.  Returns def if the key is absent. */
long long ipc_get_int(const char *json, const char *key, long long def);

/* Escape src for embedding in a JSON string value; writes into dst[cap]. */
void ipc_json_escape(const char *src, char *dst, size_t cap);

#endif
