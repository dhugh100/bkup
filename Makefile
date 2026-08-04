CC      = gcc
PKGS    = sqlite3 libzstd libsodium libssh2
CFLAGS  = -std=c11 -D_GNU_SOURCE -Wall -Wextra -O2 -g -Isrc -MMD -MP \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS))

GTK4_CFLAGS = $(shell pkg-config --cflags gtk4)
GTK4_LIBS   = $(shell pkg-config --libs gtk4)

COMMON_OBJ = $(patsubst src/%.c,obj/%.o, \
               $(wildcard src/common/*.c) $(wildcard src/platform/*.c))

# CLI object files shared by bkup and bkupd (excludes cli/main.c)
CLI_OBJ    = $(patsubst src/%.c,obj/%.o, \
               $(filter-out src/cli/main.c, $(wildcard src/cli/*.c)))

DAEMON_OBJ = $(patsubst src/%.c,obj/%.o,$(wildcard src/daemon/*.c))
GUI_OBJ    = $(patsubst src/%.c,obj/%.o,$(wildcard src/gui/*.c))

all: bin/bkup bin/bkupd bin/bkup-gui

# bkup CLI: common + shared CLI + cli main
bin/bkup: $(COMMON_OBJ) $(CLI_OBJ) obj/cli/main.o
	@mkdir -p bin
	$(CC) -o $@ $^ $(LDLIBS)

# bkupd daemon: common + shared CLI (backup/restore/verify) + daemon
bin/bkupd: $(COMMON_OBJ) $(CLI_OBJ) $(DAEMON_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $^ $(LDLIBS) -lpthread

# bkup-gui: GTK4 frontend (standalone, links only GTK4 + pthread)
bin/bkup-gui: $(GUI_OBJ)
	@mkdir -p bin
	$(CC) -o $@ $^ $(GTK4_LIBS) -lpthread

# Generic rule for common/cli/daemon sources
obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# GUI sources need GTK4 flags; suppress GtkDialog deprecation warnings
obj/gui/%.o: src/gui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GTK4_CFLAGS) -Wno-deprecated-declarations -c -o $@ $<

clean:
	rm -rf bin obj

# Restore SELinux labels on the freshly built binaries. A rebuild creates new
# inodes under /home that default to user_home_t; that drops bkupd's
# bkupd_exec_t label, so systemd no longer transitions init_t -> bkupd_t and the
# daemon hits denials on bkupd_catalog_t. Run after building (needs root):
#   sudo make relabel
relabel:
	@if command -v restorecon >/dev/null 2>&1 && selinuxenabled 2>/dev/null; then \
		restorecon -Fv bin/bkup bin/bkupd bin/bkup-gui; \
	else \
		echo "SELinux not enabled (or restorecon missing); skipping relabel"; \
	fi

# Auto-generated header dependencies (-MMD). Ensures a changed .h rebuilds
# every .o that includes it.
-include $(shell find obj -name '*.d' 2>/dev/null)

test: all
	@tests/run_tests.sh

.PHONY: all clean relabel test
