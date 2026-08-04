#ifndef BK_IPC_H
#define BK_IPC_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int   fd;
    char *config_path;
    uid_t caller_uid;          /* peer uid (SO_PEERCRED): the user being served */
} ConnArg;

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

/* Connection thread entry point.  arg must be a malloc'd ConnArg*;
   the thread frees it before returning. */
void *ipc_conn_thread(void *arg);

/* Acquire/release the global one-at-a-time operation lock. The scheduler
   uses these to serialize scheduled backups/prunes with client requests. */
void ipc_op_lock(const char *cmd);
void ipc_op_unlock(void);

#endif
