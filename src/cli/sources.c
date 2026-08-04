#include <stdio.h>

#include "commands.h"

int cmd_sources(Ctx *c, int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("%-20s %-7s %-8s %s\n", "USER", "SCOPE", "SOURCES", "REPO");
    for (int i = 0; i < c->cfg->nusers; i++) {
        User *s = &c->cfg->users[i];
        printf("%-20s %-7s %-8d %s\n",
               s->name,
               s->scope == SCOPE_SYSTEM ? "system" : "user",
               s->nsources,
               s->repo ? s->repo : "(none)");
    }
    return 0;
}
