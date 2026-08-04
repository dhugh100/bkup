#ifndef BK_SCHEDULER_H
#define BK_SCHEDULER_H

/* Start the background scheduler thread. It reads each source's backup/prune
   schedule from the config and fires them at the configured times, serializing
   with client requests through the IPC op lock. */
void scheduler_start(const char *config_path);

#endif
