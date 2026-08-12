#ifndef BK_CLIENT_H
#define BK_CLIENT_H

/* Run one command by asking bkupd to perform it, streaming its events to the
   terminal. `cmd` is the IPC verb (backup, restore, verify, prune) and argv is
   the arguments after the subcommand, parsed exactly as the local command
   parses them. `wait_sec` bounds how long the daemon queues the request behind
   a running operation: 0 fails immediately, < 0 leaves the daemon's default.
   Returns a process exit code. */
int client_run(const char *cmd, int argc, char **argv, long long wait_sec,
               const char *sock_path);

#endif
