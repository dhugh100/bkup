#ifndef BK_IPC_H
#define BK_IPC_H

#include <stddef.h>
#include <sys/types.h>

/* The wire itself (socket path, line I/O, JSON scalars) is shared with the CLI
   client; this header is the daemon-only half. */
#include "common/ipcwire.h"

typedef struct {
    int   fd;
    char *config_path;
    uid_t caller_uid;          /* peer uid (SO_PEERCRED): the user being served */
} ConnArg;

/* Connection thread entry point.  arg must be a malloc'd ConnArg*;
   the thread frees it before returning. */
void *ipc_conn_thread(void *arg);

/* Acquire/release the global one-at-a-time operation lock. The scheduler
   uses these to serialize scheduled backups/prunes with client requests. */
void ipc_op_lock(const char *cmd);
void ipc_op_unlock(void);

#endif
