#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

/* ---- GUI-side IPC helpers (blocking) ---- */

static int gui_connect(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd); return -1;
    }
    return fd;
}

static int gui_readline(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap - 1) {
        char c;
        if (read(fd, &c, 1) <= 0) return -1;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return 0;
}

static void gui_send(int fd, const char *json)
{
    size_t len = strlen(json);
    (void)write(fd, json, len);
    (void)write(fd, "\n", 1);
}

static char *jget_str(const char *json, const char *key)
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
        if (*p == '\\' && p[1] == '"')  { buf[n++] = '"';  p += 2; }
        else if (*p == '\\' && p[1] == '\\') { buf[n++] = '\\'; p += 2; }
        else if (*p == '"') break;
        else buf[n++] = *p++;
    }
    buf[n] = '\0';
    return strdup(buf);
}

static long long jget_int(const char *json, const char *key, long long def)
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

/* Escape a plain string for use as a JSON string value. */
static void json_esc(const char *src, char *dst, size_t cap)
{
    size_t i = 0, o = 0;
    while (src[i] && o + 6 < cap) {
        unsigned char c = (unsigned char)src[i++];
        if      (c == '"')  { dst[o++] = '\\'; dst[o++] = '"';  }
        else if (c == '\\') { dst[o++] = '\\'; dst[o++] = '\\'; }
        else if (c < 0x20)  { o += (size_t)snprintf(dst+o, cap-o, "\\u%04x", c); }
        else                 { dst[o++] = (char)c; }
    }
    dst[o] = '\0';
}

/* ---- List item object (backs the virtualized GtkListView model) ---- */

#define BKUP_TYPE_ENTRY (bkup_entry_get_type())
G_DECLARE_FINAL_TYPE(BkupEntry, bkup_entry, BKUP, ENTRY, GObject)

struct _BkupEntry {
    GObject    parent_instance;
    char      *path;
    int        kind;
    long long  size;
    int        deleted;
};

G_DEFINE_TYPE(BkupEntry, bkup_entry, G_TYPE_OBJECT)

static void bkup_entry_finalize(GObject *o)
{
    BkupEntry *e = BKUP_ENTRY(o);
    g_free(e->path);
    G_OBJECT_CLASS(bkup_entry_parent_class)->finalize(o);
}

static void bkup_entry_class_init(BkupEntryClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = bkup_entry_finalize;
}

static void bkup_entry_init(BkupEntry *e) { (void)e; }

static BkupEntry *bkup_entry_new(const char *path, int kind,
                                 long long size, int deleted)
{
    BkupEntry *e = g_object_new(BKUP_TYPE_ENTRY, NULL);
    e->path    = g_strdup(path ? path : "");
    e->kind    = kind;
    e->size    = size;
    e->deleted = deleted;
    return e;
}

/* ---- App state ---- */

#define MAX_SOURCES 32

typedef struct { char name[128]; int scope; } GuiSource;

typedef struct {
    GtkApplication *gapp;
    GtkWidget      *window;
    GtkWidget      *buildlist_btn;   /* header "Build List" */
    GtkWidget      *buildlist_spinner;
    GtkWidget      *recover_btn;
    GtkWidget      *backup_btn;      /* header "Backup Now" */
    GtkWidget      *backup_spinner;  /* shown only while a backup runs */
    GtkWidget      *recover_spinner; /* shown only while a restore runs */
    GtkWidget      *filter_entry;    /* live substring filter over the list */
    GtkWidget      *settings_grid;   /* right-pane read-only settings */
    GtkWidget      *status_grid;     /* right-pane repo/continuous status */
    GtkWidget      *entry_list;      /* GtkListView: flat, multi-select recover list */
    GListStore     *entry_store;     /* BkupEntry items (full, unfiltered) */
    GtkFilter      *entry_filter;    /* substring filter over paths */
    GtkSelectionModel *entry_sel;    /* GtkMultiSelection over the filter model */
    GtkWidget      *log_view;
    GtkWidget      *log_scroll;
    GtkTextBuffer  *log_buf;

    char            sock[256];
    gboolean        busy;
    gboolean        listing;         /* a Build List is in flight */

    GuiSource       sources[MAX_SOURCES];
    int             nsources;
    int             cur_source;
    gboolean        continuous_on;       /* current source's continuous setting */

    long long       asof;            /* restore cutoff epoch; 0 = now (most recent) */
} App;

static const char *cur_source_name(App *a)
{
    if (a->cur_source < 0 || a->cur_source >= a->nsources) return "";
    return a->sources[a->cur_source].name;
}

/* ---- Logging (main thread) ---- */

static void log_append(App *a, const char *text)
{
    char ts[24];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S ", &tm);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(a->log_buf, &end);
    gtk_text_buffer_insert(a->log_buf, &end, ts, -1);
    gtk_text_buffer_insert(a->log_buf, &end, text, -1);
    gtk_text_buffer_get_end_iter(a->log_buf, &end);
    gtk_text_buffer_insert(a->log_buf, &end, "\n", 1);
    GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(a->log_scroll));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

typedef struct { App *a; char text[2048]; } LogIdle;

static gboolean idle_log(gpointer p)
{
    LogIdle *d = p;
    log_append(d->a, d->text);
    free(d); return G_SOURCE_REMOVE;
}

static void emit_log(App *a, const char *lvl, const char *msg)
{
    LogIdle *d = malloc(sizeof *d);
    d->a = a;
    snprintf(d->text, sizeof d->text, "[%s] %s", lvl, msg);
    g_idle_add(idle_log, d);
}

/* Forward declarations. */
static void refresh_source(App *a);
static void populate_settings(App *a);
static void update_status(App *a);
static gboolean list_filter_func(gpointer item, gpointer data);
static void update_recover_sensitive(App *a);

/* ---- Async op thread (backup / prune / restore) ---- */

typedef struct { App *a; gboolean ok; char op[32]; } DoneIdle;

static gboolean idle_done(gpointer p)
{
    DoneIdle *d = p;
    App *a = d->a;
    a->busy = FALSE;
    if (strcmp(d->op, "backup") == 0) {
        gtk_spinner_stop(GTK_SPINNER(a->backup_spinner));
        gtk_widget_set_visible(a->backup_spinner, FALSE);
        gtk_widget_set_sensitive(a->backup_btn, TRUE);
    }
    if (strcmp(d->op, "restore") == 0) {
        gtk_spinner_stop(GTK_SPINNER(a->recover_spinner));
        gtk_widget_set_visible(a->recover_spinner, FALSE);
    }
    char msg[64];
    snprintf(msg, sizeof msg, "[%s %s]", d->op, d->ok ? "done" : "FAILED");
    log_append(a, msg);
    /* Only backup/prune change the snapshot set; refresh the right pane for
       those.  A restore writes only to the local destination and never touches
       the catalog, so refreshing after it is pointless -- and races the daemon's
       op mutex, briefly returning "busy". */
    if (d->ok && (strcmp(d->op, "backup") == 0 || strcmp(d->op, "prune") == 0))
        refresh_source(a);
    free(d); return G_SOURCE_REMOVE;
}

typedef struct { App *a; char *json; char op[32]; } OpArgs;

static void *op_thread(void *arg)
{
    OpArgs *op = arg;
    App *a = op->a;

    int fd = gui_connect(a->sock);
    if (fd < 0) {
        emit_log(a, "E", "cannot connect to bkupd (is the daemon running?)");
        DoneIdle *d = malloc(sizeof *d);
        d->a = a; d->ok = FALSE;
        snprintf(d->op, sizeof d->op, "%s", op->op);
        g_idle_add(idle_done, d);
        g_free(op->json); free(op); return NULL;
    }

    gui_send(fd, op->json);

    char line[8192];
    gboolean ok = FALSE;
    while (gui_readline(fd, line, sizeof line) == 0) {
        char *evt = jget_str(line, "event");
        if (!evt) continue;
        if (strcmp(evt, "log") == 0) {
            char *lvl = jget_str(line, "level");
            char *msg = jget_str(line, "msg");
            emit_log(a, lvl ? lvl : "I", msg ? msg : "");
            free(lvl); free(msg);
        } else if (strcmp(evt, "done") == 0) {
            ok = TRUE; free(evt); break;
        } else if (strcmp(evt, "error") == 0) {
            char *msg = jget_str(line, "msg");
            emit_log(a, "E", msg ? msg : "error");
            free(msg); free(evt); break;
        }
        free(evt);
    }
    close(fd);

    DoneIdle *d = malloc(sizeof *d);
    d->a = a; d->ok = ok;
    snprintf(d->op, sizeof d->op, "%s", op->op);
    g_idle_add(idle_done, d);
    g_free(op->json); free(op); return NULL;
}

static void run_op(App *a, const char *json, const char *op_name)
{
    if (a->busy) {
        log_append(a, "[W] busy -- wait for current operation to finish");
        return;
    }
    a->busy = TRUE;

    OpArgs *args = malloc(sizeof *args);
    args->a = a;
    args->json = g_strdup(json);
    snprintf(args->op, sizeof args->op, "%s", op_name);
    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, op_thread, args);
    pthread_attr_destroy(&attr);
}

/* ---- List rows ---- */

static void human_size(long long n, char *out, size_t cap)
{
    if (n >= 1024*1024) snprintf(out, cap, "%.1f MB", n / (1024.0*1024.0));
    else if (n >= 1024) snprintf(out, cap, "%.1f KB", n / 1024.0);
    else                snprintf(out, cap, "%lld B", n);
}

/* GtkListView factory. The view virtualizes: only on-screen rows ever get
   widgets, so a list of tens of thousands of items costs the same to display as
   a handful. `setup` builds one reusable row widget tree; `bind` points it at
   whichever BkupEntry currently occupies that slot. */
static void factory_setup(GtkSignalListItemFactory *f, GtkListItem *item,
                          gpointer u)
{
    (void)f; (void)u;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 3);   gtk_widget_set_margin_bottom(box, 3);

    GtkWidget *img = gtk_image_new();
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);

    GtkWidget *tag = gtk_label_new("deleted");
    gtk_widget_add_css_class(tag, "dim-label");

    GtkWidget *szl = gtk_label_new(NULL);
    gtk_widget_add_css_class(szl, "dim-label");

    gtk_box_append(GTK_BOX(box), img);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), tag);
    gtk_box_append(GTK_BOX(box), szl);

    g_object_set_data(G_OBJECT(box), "img", img);
    g_object_set_data(G_OBJECT(box), "lbl", lbl);
    g_object_set_data(G_OBJECT(box), "tag", tag);
    g_object_set_data(G_OBJECT(box), "szl", szl);
    gtk_list_item_set_child(item, box);
}

static void factory_bind(GtkSignalListItemFactory *f, GtkListItem *item,
                         gpointer u)
{
    (void)f; (void)u;
    GtkWidget *box = gtk_list_item_get_child(item);
    BkupEntry *e   = gtk_list_item_get_item(item);
    if (!box || !e) return;

    GtkWidget *img = g_object_get_data(G_OBJECT(box), "img");
    GtkWidget *lbl = g_object_get_data(G_OBJECT(box), "lbl");
    GtkWidget *tag = g_object_get_data(G_OBJECT(box), "tag");
    GtkWidget *szl = g_object_get_data(G_OBJECT(box), "szl");

    const char *icon = e->kind == 1 ? "folder-symbolic"
                     : e->kind == 2 ? "emblem-symbolic-link"
                                    : "text-x-generic-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(img), icon);
    gtk_label_set_text(GTK_LABEL(lbl), e->path);

    /* A deleted item (gone from the latest backup, still recoverable from an
       older version) is dimmed and tagged so it reads as not-current. */
    if (e->deleted) gtk_widget_add_css_class(lbl, "dim-label");
    else            gtk_widget_remove_css_class(lbl, "dim-label");
    gtk_widget_set_visible(tag, e->deleted);

    if (e->kind == 0) {
        char sz[32]; human_size(e->size, sz, sizeof sz);
        gtk_label_set_text(GTK_LABEL(szl), sz);
        gtk_widget_set_visible(szl, TRUE);
    } else {
        gtk_widget_set_visible(szl, FALSE);
    }
}

/* ---- Build List ---- */

typedef struct { char path[4096]; int kind; long long size; int deleted; } Entry;
typedef struct { App *a; Entry *items; int n; gboolean ok; } ListResult;
typedef struct { App *a; char source[128]; char filter[1024]; long long asof; } ListArg;

static gboolean idle_listdone(gpointer p)
{
    ListResult *lr = p;
    App *a = lr->a;

    /* Build all item objects up front, then hand them to the store in one
       splice -- a single items-changed emission instead of n. Creating objects
       is cheap (no widgets), so even tens of thousands stay well under a frame
       and the spinner keeps animating. */
    GObject **objs = lr->n ? g_new(GObject *, (size_t)lr->n) : NULL;
    for (int i = 0; i < lr->n; i++)
        objs[i] = G_OBJECT(bkup_entry_new(lr->items[i].path, lr->items[i].kind,
                                          lr->items[i].size, lr->items[i].deleted));
    g_list_store_splice(a->entry_store, 0,
                        g_list_model_get_n_items(G_LIST_MODEL(a->entry_store)),
                        (gpointer *)objs, (guint)lr->n);
    for (int i = 0; i < lr->n; i++) g_object_unref(objs[i]);
    g_free(objs);
    update_recover_sensitive(a);

    gtk_spinner_stop(GTK_SPINNER(a->buildlist_spinner));
    gtk_widget_set_visible(a->buildlist_spinner, FALSE);
    gtk_widget_set_sensitive(a->buildlist_btn, TRUE);
    gtk_widget_set_visible(a->filter_entry, TRUE);
    a->listing = FALSE;

    if (lr->ok) {
        char m[64];
        snprintf(m, sizeof m, "[built list: %d recoverable item(s)]", lr->n);
        log_append(a, m);
    }
    free(lr->items); free(lr);
    return G_SOURCE_REMOVE;
}

static void *listall_thread(void *arg)
{
    ListArg *la = arg;
    App *a = la->a;

    ListResult *lr = malloc(sizeof *lr);
    lr->a = a; lr->items = NULL; lr->n = 0; lr->ok = FALSE;
    int cap = 0;

    int fd = gui_connect(a->sock);
    if (fd < 0) {
        emit_log(a, "E", "cannot connect to bkupd");
        g_idle_add(idle_listdone, lr);
        free(la); return NULL;
    }
    char esrc[256], efilter[2048], req[4096];
    json_esc(la->source, esrc, sizeof esrc);
    json_esc(la->filter, efilter, sizeof efilter);
    snprintf(req, sizeof req,
             "{\"cmd\":\"list-all\",\"source\":\"%s\",\"filter\":\"%s\",\"asof\":%lld}",
             esrc, efilter, la->asof);
    gui_send(fd, req);

    char line[8192];
    while (gui_readline(fd, line, sizeof line) == 0) {
        char *evt = jget_str(line, "event");
        if (!evt) continue;
        gboolean stop = (strcmp(evt, "done") == 0 || strcmp(evt, "error") == 0);
        if (strcmp(evt, "done") == 0) lr->ok = TRUE;
        if (strcmp(evt, "error") == 0) {
            char *msg = jget_str(line, "msg");
            emit_log(a, "E", msg ? msg : "list failed");
            free(msg);
        }
        if (strcmp(evt, "entry") == 0) {
            if (lr->n == cap) {
                cap = cap ? cap * 2 : 1024;
                lr->items = realloc(lr->items, (size_t)cap * sizeof *lr->items);
            }
            Entry *e = &lr->items[lr->n++];
            char *p = jget_str(line, "path");
            snprintf(e->path, sizeof e->path, "%s", p ? p : "");
            e->kind    = (int)jget_int(line, "kind", 0);
            e->size    = jget_int(line, "size", 0);
            e->deleted = (int)jget_int(line, "deleted", 0);
            free(p);
        }
        free(evt);
        if (stop) break;
    }
    close(fd);

    g_idle_add(idle_listdone, lr);
    free(la); return NULL;
}

static void on_buildlist_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    if (a->busy)    { log_append(a, "[W] busy -- wait for current operation"); return; }
    if (a->listing) return;
    if (a->cur_source < 0) { log_append(a, "[W] no source selected"); return; }

    /* The filter box doubles as the build scope: a non-empty value is sent to
       the daemon so only matching paths are materialized (and still refines the
       built list live afterwards). Empty builds the full list. */
    const char *filter = gtk_editable_get_text(GTK_EDITABLE(a->filter_entry));

    a->listing = TRUE;
    gtk_widget_set_sensitive(a->buildlist_btn, FALSE);
    gtk_widget_set_visible(a->buildlist_spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(a->buildlist_spinner));
    {
        char m[160];
        if (a->asof) {
            time_t t = (time_t)a->asof;
            struct tm tm; char tb[48];
            localtime_r(&t, &tm);
            strftime(tb, sizeof tb, "%Y-%m-%d", &tm);
            snprintf(m, sizeof m, "[I] building recoverable file list%s as of %s...",
                     (filter && filter[0]) ? " (filtered)" : "", tb);
        } else {
            snprintf(m, sizeof m, "[I] building recoverable file list%s...",
                     (filter && filter[0]) ? " (filtered)" : "");
        }
        log_append(a, m);
    }

    ListArg *la = malloc(sizeof *la);
    la->a = a;
    snprintf(la->source, sizeof la->source, "%s", cur_source_name(a));
    snprintf(la->filter, sizeof la->filter, "%s", filter ? filter : "");
    la->asof = a->asof;
    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, listall_thread, la);
    pthread_attr_destroy(&attr);
}

/* ---- Filter ---- */

static gboolean list_filter_func(gpointer item, gpointer data)
{
    App *a = data;
    const char *f = gtk_editable_get_text(GTK_EDITABLE(a->filter_entry));
    if (!f || !f[0]) return TRUE;
    BkupEntry *e = item;
    if (!e->path) return TRUE;
    char *hay  = g_utf8_casefold(e->path, -1);
    char *need = g_utf8_casefold(f, -1);
    gboolean match = strstr(hay, need) != NULL;
    g_free(hay); g_free(need);
    return match;
}

static void on_filter_changed(GtkEditable *e, gpointer data)
{
    (void)e;
    App *a = data;
    gtk_filter_changed(a->entry_filter, GTK_FILTER_CHANGE_DIFFERENT);
    update_recover_sensitive(a);
}

/* Recover acts on the current selection, so it stays grayed out until at least
   one row is selected (and from startup, where the list is empty). */
static void update_recover_sensitive(App *a)
{
    gboolean any = FALSE;
    if (a->entry_sel) {
        GtkBitset *sel = gtk_selection_model_get_selection(a->entry_sel);
        any = !gtk_bitset_is_empty(sel);
        gtk_bitset_unref(sel);
    }
    gtk_widget_set_sensitive(a->recover_btn, any);
}

static void on_selection_changed(GtkSelectionModel *m, guint pos, guint n,
                                 gpointer data)
{
    (void)m; (void)pos; (void)n;
    update_recover_sensitive(data);
}

/* ---- As-of date ----

   The as-of cutoff is a build-time choice, so there is no standing control for
   it: it lives in a fly-out hung off the Build List button. Opening Build List
   offers a calendar plus "Most recent (now)"; picking either sets the cutoff and
   immediately builds. Once a dated cutoff is active, a reset affordance appears
   in the header (showing the active date) to return to "now" and rebuild. */

/* Dismiss the Build List fly-out, if it is open. */
static void asof_popdown(App *a)
{
    GtkPopover *pop = gtk_menu_button_get_popover(GTK_MENU_BUTTON(a->buildlist_btn));
    if (pop) gtk_popover_popdown(pop);
}

/* Reset the as-of back to "now" (most recent): clear the cutoff and hide the
   reset affordance. Does not rebuild on its own -- callers decide whether a
   rebuild follows. */
static void asof_reset_now(App *a)
{
    a->asof = 0;
}

static void on_asof_now(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    asof_reset_now(a);
    asof_popdown(a);
    on_buildlist_clicked(NULL, a);
}

static void on_asof_day_selected(GtkCalendar *cal, gpointer data)
{
    App *a = data;
    GDateTime *day  = gtk_calendar_get_date(cal);          /* midnight, local */
    GDateTime *next = g_date_time_add_days(day, 1);
    a->asof = (long long)g_date_time_to_unix(next) - 1;    /* end of selected day */

    g_date_time_unref(day);
    g_date_time_unref(next);

    asof_popdown(a);
    on_buildlist_clicked(NULL, a);
}


/* The Build List fly-out: a calendar plus a "Most recent (now)" button. Picking
   either sets the cutoff and builds immediately (see the handlers above). */
static GtkWidget *build_asof_popover(App *a)
{
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8); gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);   gtk_widget_set_margin_bottom(box, 8);

    GtkWidget *cal = gtk_calendar_new();
    g_signal_connect(cal, "day-selected", G_CALLBACK(on_asof_day_selected), a);
    gtk_box_append(GTK_BOX(box), cal);

    GtkWidget *nowb = gtk_button_new_with_label("Most recent (now)");
    g_signal_connect(nowb, "clicked", G_CALLBACK(on_asof_now), a);
    gtk_box_append(GTK_BOX(box), nowb);

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    return pop;
}

/* ---- Recover ---- */

typedef struct {
    App       *a;
    GPtrArray *dirs;          /* char* (full paths) */
    GPtrArray *files;
    GtkWidget *dest_entry;
} RecoverDlg;

/* Append "p1","p2",... (JSON-escaped) to gs. */
static void append_json_array(GString *gs, GPtrArray *paths)
{
    for (guint i = 0; i < paths->len; i++) {
        char e[4096];
        json_esc(paths->pdata[i], e, sizeof e);
        if (i) g_string_append_c(gs, ',');
        g_string_append_printf(gs, "\"%s\"", e);
    }
}

static void on_recover_response(GtkDialog *dlg, gint resp, gpointer data)
{
    RecoverDlg *rd = data;
    App *a = rd->a;
    if (resp == GTK_RESPONSE_ACCEPT) {
        const char *dest = gtk_editable_get_text(GTK_EDITABLE(rd->dest_entry));
        if (dest && dest[0]) {
            long long asof = a->asof ? a->asof : (long long)time(NULL);
            char esrc[256], edest[4096];
            json_esc(cur_source_name(a), esrc, sizeof esrc);
            json_esc(dest, edest, sizeof edest);

            GString *j = g_string_new(NULL);
            g_string_append_printf(j,
                "{\"cmd\":\"restore\",\"source\":\"%s\",\"dest\":\"%s\",\"asof\":%lld,"
                "\"dirs\":[", esrc, edest, asof);
            append_json_array(j, rd->dirs);
            g_string_append(j, "],\"files\":[");
            append_json_array(j, rd->files);
            g_string_append(j, "]}");

            time_t t = (time_t)asof;
            struct tm tm; char tb[48];
            localtime_r(&t, &tm);
            strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", &tm);
            char msg[256];
            snprintf(msg, sizeof msg, "[I] recovering %u item(s) as of %s to %s",
                     rd->dirs->len + rd->files->len, tb, dest);
            log_append(a, msg);

            gtk_widget_set_visible(a->recover_spinner, TRUE);
            gtk_spinner_start(GTK_SPINNER(a->recover_spinner));
            run_op(a, j->str, "restore");
            g_string_free(j, TRUE);
        }
    }
    g_ptr_array_free(rd->dirs, TRUE);
    g_ptr_array_free(rd->files, TRUE);
    free(rd);
    gtk_window_destroy(GTK_WINDOW(dlg));
}

static void on_recover_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    GtkBitset *sel = gtk_selection_model_get_selection(a->entry_sel);
    if (gtk_bitset_is_empty(sel)) {
        gtk_bitset_unref(sel);
        log_append(a, "[W] select one or more files/directories to recover");
        return;
    }

    RecoverDlg *rd = malloc(sizeof *rd);
    rd->a = a;
    rd->dirs  = g_ptr_array_new_with_free_func(g_free);
    rd->files = g_ptr_array_new_with_free_func(g_free);
    const char *last_path = NULL;

    GtkBitsetIter it; guint pos;
    if (gtk_bitset_iter_init_first(&it, sel, &pos)) {
        do {
            BkupEntry *e = g_list_model_get_item(G_LIST_MODEL(a->entry_sel), pos);
            if (!e) continue;
            last_path = e->path;
            if (e->kind == 1) g_ptr_array_add(rd->dirs, g_strdup(e->path));
            else              g_ptr_array_add(rd->files, g_strdup(e->path));
            g_object_unref(e);
        } while (gtk_bitset_iter_next(&it, &pos));
    }
    gtk_bitset_unref(sel);

    guint count = rd->dirs->len + rd->files->len;

    GtkWidget *dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Recover");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(a->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
        "_Cancel", GTK_RESPONSE_CANCEL, "_Recover", GTK_RESPONSE_ACCEPT, NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(content), 8);
    gtk_widget_set_margin_start(content, 16); gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);   gtk_widget_set_margin_bottom(content, 8);

    char asbuf[64];
    if (a->asof) {
        time_t t = (time_t)a->asof;
        struct tm tm; char tb[48];
        localtime_r(&t, &tm);
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", &tm);
        snprintf(asbuf, sizeof asbuf, "as of %s", tb);
    } else {
        snprintf(asbuf, sizeof asbuf, "as of now (most recent)");
    }
    char hdr[128];
    snprintf(hdr, sizeof hdr, "Recover %u selected item(s)\n%s", count, asbuf);
    GtkWidget *l = gtk_label_new(hdr);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_box_append(GTK_BOX(content), l);

    gtk_box_append(GTK_BOX(content), gtk_label_new("Restore into directory:"));
    rd->dest_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(rd->dest_entry), "/path/to/destination");
    /* For a single item default to its original parent (so leaving dest unchanged
       restores in place: each item lands at dest/<name>). For several items pick
       a neutral landing dir. */
    char defbuf[4096] = "/tmp/restore";
    if (count == 1 && last_path) {
        snprintf(defbuf, sizeof defbuf, "%s", last_path);
        char *slash = strrchr(defbuf, '/');
        if (slash == defbuf) defbuf[1] = '\0';     /* item directly under "/" */
        else if (slash) *slash = '\0';
    }
    gtk_editable_set_text(GTK_EDITABLE(rd->dest_entry), defbuf);
    gtk_widget_set_size_request(rd->dest_entry, 380, -1);
    gtk_box_append(GTK_BOX(content), rd->dest_entry);

    g_signal_connect(dlg, "response", G_CALLBACK(on_recover_response), rd);
    gtk_widget_show(dlg);
}

/* ---- Source loading ---- */

static void load_sources(App *a)
{
    a->nsources = 0;
    a->cur_source = -1;

    int fd = gui_connect(a->sock);
    if (fd < 0) { log_append(a, "[E] cannot connect to bkupd"); return; }
    gui_send(fd, "{\"cmd\":\"list-sources\"}");

    char line[4096];
    while (gui_readline(fd, line, sizeof line) == 0) {
        char *evt = jget_str(line, "event");
        if (!evt) continue;
        gboolean stop = (strcmp(evt, "done") == 0 || strcmp(evt, "error") == 0);
        if (strcmp(evt, "source") == 0 && a->nsources < MAX_SOURCES) {
            char *nm = jget_str(line, "name");
            snprintf(a->sources[a->nsources].name,
                     sizeof a->sources[0].name, "%s", nm ? nm : "");
            a->sources[a->nsources].scope = (int)jget_int(line, "scope", 0);
            a->nsources++;
            free(nm);
        }
        free(evt);
        if (stop) break;
    }
    close(fd);

    if (a->nsources > 0) {
        a->cur_source = 0;
        refresh_source(a);
    }
}

/* Refresh the right-pane Settings + Status for the current source. The flat
   recovery list is built on demand (Build List), not here. */
static void refresh_source(App *a)
{
    populate_settings(a);
    update_status(a);
}

/* ---- Header "Backup Now" ---- */

static GtkWidget *g_menu_popover;   /* so menu actions can pop it down */

static void on_backup_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    if (a->busy) { log_append(a, "[W] busy -- wait for current operation"); return; }
    gtk_widget_set_sensitive(a->backup_btn, FALSE);
    gtk_widget_set_visible(a->backup_spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(a->backup_spinner));
    char esrc[256], json[512];
    json_esc(cur_source_name(a), esrc, sizeof esrc);
    snprintf(json, sizeof json, "{\"cmd\":\"backup\",\"source\":\"%s\"}", esrc);
    log_append(a, "[I] starting backup...");
    run_op(a, json, "backup");
}

/* Remove every child of a grid so it can be refilled. */
static void clear_grid(GtkWidget *grid)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(grid)))
        gtk_grid_remove(GTK_GRID(grid), c);
}

/* Append a "Label: value" row to a settings grid. */
static void settings_row(GtkWidget *grid, int row, const char *label, const char *val)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_add_css_class(l, "dim-label");
    GtkWidget *v = gtk_label_new(val && val[0] ? val : "-");
    gtk_label_set_xalign(GTK_LABEL(v), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_wrap(GTK_LABEL(v), TRUE);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
}

/* Fill the right-pane Settings grid from source-info (read-only). */
static void populate_settings(App *a)
{
    clear_grid(a->settings_grid);
    if (a->cur_source < 0) return;

    /* Fetch source-info synchronously. */
    char server[512]="", repo[4096]="", db[4096]="", backup[256]="", prune[256]="";
    char roots[8192]=""; int scope=0;
    int kl=0,kd=0,kw=0,km=0,ky=0;
    int fd = gui_connect(a->sock);
    if (fd < 0) { log_append(a, "[E] cannot connect to bkupd"); return; }
    char esrc[256], req[512];
    json_esc(cur_source_name(a), esrc, sizeof esrc);
    snprintf(req, sizeof req, "{\"cmd\":\"source-info\",\"source\":\"%s\"}", esrc);
    gui_send(fd, req);
    char line[8192];
    while (gui_readline(fd, line, sizeof line) == 0) {
        char *evt = jget_str(line, "event");
        if (!evt) continue;
        gboolean stop = (strcmp(evt, "done") == 0 || strcmp(evt, "error") == 0);
        if (strcmp(evt, "info") == 0) {
            char *v;
            if ((v = jget_str(line, "server"))) { snprintf(server, sizeof server, "%s", v); free(v); }
            if ((v = jget_str(line, "repo")))   { snprintf(repo, sizeof repo, "%s", v); free(v); }
            if ((v = jget_str(line, "db")))     { snprintf(db, sizeof db, "%s", v); free(v); }
            if ((v = jget_str(line, "backup"))) { snprintf(backup, sizeof backup, "%s", v); free(v); }
            if ((v = jget_str(line, "prune")))  { snprintf(prune, sizeof prune, "%s", v); free(v); }
            scope = (int)jget_int(line, "scope", 0);
            a->continuous_on = jget_int(line, "continuous", 1) != 0;
            kl = (int)jget_int(line, "keep_last", 0);
            kd = (int)jget_int(line, "keep_daily", 0);
            kw = (int)jget_int(line, "keep_weekly", 0);
            km = (int)jget_int(line, "keep_monthly", 0);
            ky = (int)jget_int(line, "keep_yearly", 0);
        } else if (strcmp(evt, "root") == 0) {
            char *p = jget_str(line, "path");
            if (p) {
                if (roots[0]) g_strlcat(roots, "\n", sizeof roots);
                g_strlcat(roots, p, sizeof roots);
                free(p);
            }
        }
        free(evt);
        if (stop) break;
    }
    close(fd);

    char keepbuf[128];
    snprintf(keepbuf, sizeof keepbuf,
             "last %d, daily %d, weekly %d, monthly %d, yearly %d",
             kl, kd, kw, km, ky);
    int r = 0;
    settings_row(a->settings_grid, r++, "Source",   cur_source_name(a));
    settings_row(a->settings_grid, r++, "Scope",    scope ? "system" : "user");
    settings_row(a->settings_grid, r++, "Continuous", a->continuous_on ? "on" : "off");
    settings_row(a->settings_grid, r++, "Server",   server);
    settings_row(a->settings_grid, r++, "Repo",     repo);
    settings_row(a->settings_grid, r++, "Catalog",  db);
    settings_row(a->settings_grid, r++, "Roots",    roots);
    settings_row(a->settings_grid, r++, "Backup",   backup);
    settings_row(a->settings_grid, r++, "Prune",    prune);
    settings_row(a->settings_grid, r++, "Retention", keepbuf);
}

/* Append a "Label: value" row to the status grid. */
static void status_row(GtkWidget *grid, int row, const char *label, const char *val)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_add_css_class(l, "dim-label");
    GtkWidget *v = gtk_label_new(val);
    gtk_label_set_xalign(GTK_LABEL(v), 0.0f);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
}

/* Fill the right-pane Status grid: continuous on/off + repo statistics fetched
   from the daemon's repo-stats command. */
static void update_status(App *a)
{
    clear_grid(a->status_grid);
    if (a->cur_source < 0) return;

    long long sched = 0, cont = 0, last_cont = 0, stored = 0, logical = 0;
    int fd = gui_connect(a->sock);
    if (fd < 0) { log_append(a, "[E] cannot connect to bkupd"); return; }
    char esrc[256], req[512];
    json_esc(cur_source_name(a), esrc, sizeof esrc);
    snprintf(req, sizeof req, "{\"cmd\":\"repo-stats\",\"source\":\"%s\"}", esrc);
    gui_send(fd, req);
    char line[4096];
    while (gui_readline(fd, line, sizeof line) == 0) {
        char *evt = jget_str(line, "event");
        if (!evt) continue;
        gboolean stop = (strcmp(evt, "done") == 0 || strcmp(evt, "error") == 0);
        if (strcmp(evt, "stats") == 0) {
            sched     = jget_int(line, "scheduled", 0);
            cont      = jget_int(line, "continuous", 0);
            last_cont = jget_int(line, "last_continuous", 0);
            stored    = jget_int(line, "stored_bytes", 0);
            logical   = jget_int(line, "logical_bytes", 0);
        }
        free(evt);
        if (stop) break;
    }
    close(fd);

    int r = 0;
    status_row(a->status_grid, r++, "Continuous", a->continuous_on ? "on" : "off");

    char buf[64];
    snprintf(buf, sizeof buf, "%lld scheduled, %lld continuous", sched, cont);
    status_row(a->status_grid, r++, "Snapshots", buf);

    if (last_cont > 0) {
        time_t t = (time_t)last_cont;
        struct tm tm; char tb[48];
        localtime_r(&t, &tm);
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", &tm);
        status_row(a->status_grid, r++, "Last continuous", tb);
    } else {
        status_row(a->status_grid, r++, "Last continuous", "(none)");
    }

    char szb[32]; human_size(stored, szb, sizeof szb);
    status_row(a->status_grid, r++, "Repo size", szb);

    if (stored > 0) {
        snprintf(buf, sizeof buf, "%.1fx", (double)logical / (double)stored);
        status_row(a->status_grid, r++, "Dedup + compression", buf);
    }
}

/* Periodic refresh of the right-pane status. The daemon makes continuous
   backups on its own schedule; without this poll the status pane would stay
   frozen. Skipped while an op runs (it would race the daemon's op mutex) or with
   no source selected. The recovery list is left alone. */
#define STATUS_REFRESH_SEC 20
static gboolean tick_refresh(gpointer data)
{
    App *a = data;
    if (!a->busy && a->cur_source >= 0)
        update_status(a);
    return G_SOURCE_CONTINUE;
}

static void menu_about(GtkButton *btn, gpointer data)
{
    (void)btn;
    App *a = data;
    gtk_popover_popdown(GTK_POPOVER(g_menu_popover));
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
        "bkup\n\nChunked, encrypted, deduplicated backup.\n"
        "Open Build List, pick an \"as of\" date (or most recent), select items, "
        "then Recover.");
    g_signal_connect(d, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_widget_show(d);
}

static GtkWidget *menu_item(const char *label, GCallback cb, App *a)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)), GTK_ALIGN_START);
    g_signal_connect(b, "clicked", cb, a);
    return b;
}

static GtkWidget *build_menu_button(App *a)
{
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "open-menu-symbolic");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box, 4); gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 4);   gtk_widget_set_margin_bottom(box, 4);
    gtk_box_append(GTK_BOX(box), menu_item("About", G_CALLBACK(menu_about), a));

    g_menu_popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(g_menu_popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), g_menu_popover);
    return mb;
}

/* ---- UI build ---- */

static void activate(GtkApplication *gapp, gpointer data)
{
    App *a = data;
    a->gapp = gapp;

    a->window = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(a->window), "bkup");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 1000, 680);

    /* Header bar: Build List + Recover (left), title centered, spinner + Backup
       Now + hamburger (right). */
    GtkWidget *hbar = gtk_header_bar_new();

    /* Build List is itself the as-of fly-out: opening it offers a calendar and
       "Most recent (now)", and picking either builds the list as of that point. */
    a->buildlist_btn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(a->buildlist_btn), "Build List");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(a->buildlist_btn),
                                build_asof_popover(a));
    gtk_widget_add_css_class(a->buildlist_btn, "suggested-action");
    gtk_widget_set_tooltip_text(a->buildlist_btn,
        "List recoverable files: pick an as-of date, or \"Most recent (now)\".");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), a->buildlist_btn);

    a->buildlist_spinner = gtk_spinner_new();
    gtk_widget_set_visible(a->buildlist_spinner, FALSE);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), a->buildlist_spinner);

    a->recover_btn = gtk_button_new_with_label("Recover");
    /* Grayed out until a list is built and rows are selected. */
    gtk_widget_set_sensitive(a->recover_btn, FALSE);
    g_signal_connect(a->recover_btn, "clicked", G_CALLBACK(on_recover_clicked), a);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), a->recover_btn);

    a->recover_spinner = gtk_spinner_new();
    gtk_widget_set_visible(a->recover_spinner, FALSE);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), a->recover_spinner);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), build_menu_button(a));

    a->backup_btn = gtk_button_new_with_label("Backup Now");
    g_signal_connect(a->backup_btn, "clicked", G_CALLBACK(on_backup_clicked), a);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), a->backup_btn);

    a->backup_spinner = gtk_spinner_new();
    gtk_widget_set_visible(a->backup_spinner, FALSE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), a->backup_spinner);

    gtk_window_set_titlebar(GTK_WINDOW(a->window), hbar);

    /* Body: horizontal paned (left recovery / right settings+history)
       above a full-width log. */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(a->window), root);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 560);
    gtk_box_append(GTK_BOX(root), paned);

    /* ---- Left pane: recovery ---- */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    a->filter_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(a->filter_entry, TRUE);
    gtk_widget_set_margin_start(a->filter_entry, 12);
    gtk_widget_set_margin_end(a->filter_entry, 12);
    gtk_widget_set_margin_bottom(a->filter_entry, 6);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(a->filter_entry),
                                   "path substring (scopes Build List, filters live)");
    gtk_widget_set_tooltip_text(a->filter_entry,
        "Substring matched against the path. Set before Build List to fetch only "
        "matching files (faster); also filters the built list live.");
    g_signal_connect(a->filter_entry, "search-changed",
                     G_CALLBACK(on_filter_changed), a);
    gtk_box_append(GTK_BOX(left), a->filter_entry);
    gtk_widget_set_visible(a->filter_entry, FALSE);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    /* Virtualized list: store -> filter model -> multi-selection -> view.
       Only on-screen rows are ever instantiated, so tens of thousands of
       items display and scroll without freezing the main loop. */
    a->entry_store = g_list_store_new(BKUP_TYPE_ENTRY);
    a->entry_filter = GTK_FILTER(gtk_custom_filter_new(list_filter_func, a, NULL));
    GtkFilterListModel *fm =
        gtk_filter_list_model_new(G_LIST_MODEL(a->entry_store), a->entry_filter);
    a->entry_sel = GTK_SELECTION_MODEL(gtk_multi_selection_new(G_LIST_MODEL(fm)));
    g_signal_connect(a->entry_sel, "selection-changed",
                     G_CALLBACK(on_selection_changed), a);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(factory_setup), a);
    g_signal_connect(factory, "bind",  G_CALLBACK(factory_bind),  a);

    a->entry_list = gtk_list_view_new(a->entry_sel, factory);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), a->entry_list);
    gtk_box_append(GTK_BOX(left), sw);

    gtk_paned_set_start_child(GTK_PANED(paned), left);

    /* ---- Right pane: settings + status ---- */
    GtkWidget *rsw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rsw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(right, 12); gtk_widget_set_margin_end(right, 12);
    gtk_widget_set_margin_top(right, 8);    gtk_widget_set_margin_bottom(right, 8);

    GtkWidget *sh = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sh), "<b>Settings</b>");
    gtk_label_set_xalign(GTK_LABEL(sh), 0.0f);
    gtk_box_append(GTK_BOX(right), sh);
    GtkWidget *snote = gtk_label_new("Read-only. Edit /etc/bkup.conf as admin to change.");
    gtk_label_set_xalign(GTK_LABEL(snote), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(snote), TRUE);
    gtk_widget_add_css_class(snote, "dim-label");
    gtk_box_append(GTK_BOX(right), snote);
    a->settings_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(a->settings_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(a->settings_grid), 16);
    gtk_box_append(GTK_BOX(right), a->settings_grid);

    gtk_box_append(GTK_BOX(right), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *lh = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lh), "<b>Status</b>");
    gtk_label_set_xalign(GTK_LABEL(lh), 0.0f);
    gtk_box_append(GTK_BOX(right), lh);
    a->status_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(a->status_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(a->status_grid), 16);
    gtk_box_append(GTK_BOX(right), a->status_grid);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rsw), right);
    gtk_paned_set_end_child(GTK_PANED(paned), rsw);

    /* ---- Bottom: full-width log ---- */
    GtkWidget *log_frame = gtk_frame_new("Log");
    gtk_widget_set_margin_start(log_frame, 8); gtk_widget_set_margin_end(log_frame, 8);
    gtk_widget_set_margin_top(log_frame, 2);   gtk_widget_set_margin_bottom(log_frame, 8);
    a->log_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(a->log_scroll, -1, 130);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(a->log_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    a->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(a->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(a->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(a->log_view), TRUE);
    a->log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->log_view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(a->log_scroll), a->log_view);
    gtk_frame_set_child(GTK_FRAME(log_frame), a->log_scroll);
    gtk_box_append(GTK_BOX(root), log_frame);

    load_sources(a);
    g_timeout_add_seconds(STATUS_REFRESH_SEC, tick_refresh, a);
    gtk_window_present(GTK_WINDOW(a->window));
    gtk_widget_grab_focus(a->buildlist_btn);
}

int main(int argc, char **argv)
{
    App *a = calloc(1, sizeof *a);

    const char *sock = NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-s") && i + 1 < argc) sock = argv[++i];

    if (sock) {
        snprintf(a->sock, sizeof a->sock, "%s", sock);
    } else {
        /* The single root daemon listens on a fixed shared socket; override
           with -s for a dev daemon on a private path. */
        snprintf(a->sock, sizeof a->sock, "/run/bkupd.sock");
    }

    GtkApplication *gapp =
        gtk_application_new("org.bkup.gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gapp, "activate", G_CALLBACK(activate), a);
    int rc = g_application_run(G_APPLICATION(gapp), 0, NULL);
    g_object_unref(gapp);
    free(a);
    return rc;
}
