#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <ncurses.h>

#include "sys_dashboard.h"
#include "ui.h"
#include "utils.h"

// Globale Variablen
char my_services[MAX_SERVICES][MAX_LINE];
int  num_my_services = 0;

char all_services[MAX_SERVICES][MAX_LINE];
int  num_all_services = 0;

// Cache-Definition
char summary_cache[MAX_SERVICES][MAX_LINE];
int  cache_valid[MAX_SERVICES];
time_t cache_timestamp[MAX_SERVICES];
#define CACHE_TTL_SECONDS 5

const char *DEFAULT_SERVICES[DEFAULT_SERVICES_COUNT] = {
    "trainee_trainer-gunicorn.service",
    "stock-predictor.service",
    "dashboard_sheep.service"
};

const char *sudo_flag = "";

// Resize signal handler
volatile sig_atomic_t need_resize = 0;
void resize_handler(int sig) {
    (void)sig;
    need_resize = 1;
}

// --------------------------------------------------
// Helper - ROBUST
// --------------------------------------------------

void init_sudo_flag(void) {
    if (geteuid() == 0)
        sudo_flag = "";
    else
        sudo_flag = "sudo ";
}

/* Safely execute command and capture output.
   Reads in chunks to avoid buffer overflow.
   Returns 0 on success, -1 on popen failure. */
int execute_cmd(const char *cmd, char *output, size_t max_output) {
    if (!output || max_output == 0) return -1;

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    size_t total = 0;
    output[0] = '\0';

    char buf[256];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        size_t n = strlen(buf);
        if (total + n >= max_output - 1) break;
        memcpy(output + total, buf, n + 1);
        total += n;
    }

    if (total > 0 && output[total - 1] == '\n') {
        output[--total] = '\0';
    }
    (void)pclose(fp);
    return 0;
}

int exec_simple(const char *cmd) {
    int ret = system(cmd);
    if (WIFEXITED(ret)) {
        return WEXITSTATUS(ret);
    }
    return -1;
}

void check_systemctl(void) {
    if (exec_simple("which systemctl >/dev/null 2>&1") != 0) {
        printf("%sFehler:%s 'systemctl' wurde nicht gefunden. Laeuft hier kein systemd?\n",
               "\033[31m", "\033[0m");
        exit(1);
    }
}

void press_enter_cli(void) {
    printf("\n%sWeiter mit [Enter]...%s\n", "\033[2m", "\033[0m");
    char dummy[64];
    if (fgets(dummy, sizeof(dummy), stdin) == NULL) return;
}

// --------------------------------------------------
// Cache Management
// --------------------------------------------------

void invalidate_cache(void) {
    for (int i = 0; i < MAX_SERVICES; i++) {
        cache_valid[i] = 0;
    }
}

void invalidate_service_cache(const char *svc) {
    for (int i = 0; i < num_my_services; i++) {
        if (strcmp(my_services[i], svc) == 0) {
            cache_valid[i] = 0;
            break;
        }
    }
    // Also invalidate all_services with same name
    for (int i = 0; i < num_all_services; i++) {
        if (strcmp(all_services[i], svc) == 0) {
            // We use negative indices for all_services to avoid collision
            cache_valid[MAX_SERVICES - 1 - i] = 0;
            break;
        }
    }
}

// --------------------------------------------------
// Config Laden/Speichern
// --------------------------------------------------

void load_services(const char *home) {
    char config_path[MAX_LINE];
    snprintf(config_path, sizeof(config_path), CONFIG_FILE, home);
    num_my_services = 0;

    FILE *fp = fopen(config_path, "r");
    if (fp) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;
            if (num_my_services < MAX_SERVICES) {
                strncpy(my_services[num_my_services], line, MAX_LINE - 1);
                my_services[num_my_services][MAX_LINE - 1] = '\0';
                num_my_services++;
            }
        }
        fclose(fp);
    } else {
        // Defaults
        for (int i = 0; i < DEFAULT_SERVICES_COUNT && num_my_services < MAX_SERVICES; i++) {
            strncpy(my_services[num_my_services], DEFAULT_SERVICES[i], MAX_LINE - 1);
            my_services[num_my_services][MAX_LINE - 1] = '\0';
            num_my_services++;
        }
        save_services(home);
    }
    invalidate_cache();
}

void save_services(const char *home) {
    char config_path[MAX_LINE];
    snprintf(config_path, sizeof(config_path), CONFIG_FILE, home);

    char dir_path[MAX_LINE];
    snprintf(dir_path, sizeof(dir_path), "%s/.config/sys-dashboard", home);
    char mkdir_cmd[MAX_LINE + 32];
    int len = snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", dir_path);
    if (len > 0 && (size_t)len < sizeof(mkdir_cmd)) {
        system(mkdir_cmd);
    }

    FILE *fp = fopen(config_path, "w");
    if (!fp) {
        fprintf(stderr, "%sFehler: Konnte Config-Datei nicht schreiben: %s%s\n",
                "\033[31m", config_path, "\033[0m");
        return;
    }

    fprintf(fp, "# Eigene systemd-Services fuer das Dashboard\n");
    fprintf(fp, "# Eine Unit pro Zeile, z.B.:\n");
    fprintf(fp, "# trainee_trainer-gunicorn.service\n\n");

    for (int i = 0; i < num_my_services; ++i) {
        fprintf(fp, "%s\n", my_services[i]);
    }
    fclose(fp);
}

// --------------------------------------------------
// Farben (CLI, nicht ncurses)
// --------------------------------------------------

static const char *OK_COLOR = "\033[32m";
static const char *WARN_COLOR = "\033[33m";
static const char *ERR_COLOR = "\033[31m";
static const char *HEADER_COLOR = "\033[1;36m";
static const char *DIM_COLOR = "\033[2m";
static const char *RESET_COLOR = "\033[0m";

void status_color(const char *s, char *buf, size_t bufsize) {
    if (strcmp(s, "active") == 0) {
        snprintf(buf, bufsize, "%s%s%s", OK_COLOR, s, RESET_COLOR);
    } else if (strcmp(s, "inactive") == 0) {
        snprintf(buf, bufsize, "%s%s%s", DIM_COLOR, s, RESET_COLOR);
    } else if (strcmp(s, "failed") == 0) {
        snprintf(buf, bufsize, "%s%s%s", ERR_COLOR, s, RESET_COLOR);
    } else if (strcmp(s, "activating") == 0 || strcmp(s, "deactivating") == 0) {
        snprintf(buf, bufsize, "%s%s%s", WARN_COLOR, s, RESET_COLOR);
    } else {
        snprintf(buf, bufsize, "%s%s%s", WARN_COLOR, (s ? s : "unknown"), RESET_COLOR);
    }
}

void enabled_color(const char *s, char *buf, size_t bufsize) {
    if (strcmp(s, "enabled") == 0) {
        snprintf(buf, bufsize, "%s%s%s", OK_COLOR, s, RESET_COLOR);
    } else if (strcmp(s, "disabled") == 0) {
        snprintf(buf, bufsize, "%s%s%s", DIM_COLOR, s, RESET_COLOR);
    } else if (strcmp(s, "static") == 0 || strcmp(s, "indirect") == 0 || strcmp(s, "generated") == 0) {
        snprintf(buf, bufsize, "%s%s%s", WARN_COLOR, s, RESET_COLOR);
    } else {
        snprintf(buf, bufsize, "%s%s%s", WARN_COLOR, (s ? s : "unknown"), RESET_COLOR);
    }
}

// --------------------------------------------------
// Scope Detection - OPTIMIZED: single systemctl show call
// --------------------------------------------------
/* Old: 2x list-unit-files (slow, parses full table)
   New: 1x systemctl show LoadState (fast, single value)
   - LoadState = "loaded" => found (try system first)
   - If not found system-wide, try --user
   - We also check if the pid is valid to determine scope */
char *detect_scope(const char *svc) {
    static char scope_buf[16];
    char cmd[MAX_LINE];
    char out[64];

    out[0] = '\0';

    /* Test system scope - use show LoadState (much faster than list-unit-files) */
    snprintf(cmd, sizeof(cmd),
             "systemctl show -p LoadState --value \"%s\" 2>/dev/null", svc);
    execute_cmd(cmd, out, sizeof(out));

    if (strcmp(out, "loaded") == 0) {
        strcpy(scope_buf, "system");
        return scope_buf;
    }

    /* Test user scope */
    out[0] = '\0';
    snprintf(cmd, sizeof(cmd),
             "systemctl --user show -p LoadState --value \"%s\" 2>/dev/null", svc);
    execute_cmd(cmd, out, sizeof(out));

    if (strcmp(out, "loaded") == 0) {
        strcpy(scope_buf, "user");
        return scope_buf;
    }

    strcpy(scope_buf, "none");
    return scope_buf;
}

// --------------------------------------------------
// Port Detection - OPTIMIZED: use ss instead of netstat
// --------------------------------------------------
char *guess_port(const char *svc, const char *scope) {
    static char port_buf[16];
    strcpy(port_buf, "-");

    if (strcmp(scope, "none") == 0) {
        return port_buf;
    }

    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");

    // MainPID holen
    char cmd[MAX_LINE];
    char pid_str[32] = {0};

    snprintf(cmd, sizeof(cmd),
             "systemctl %s show -p MainPID --value \"%s\" 2>/dev/null",
             user_flag, svc);
    if (execute_cmd(cmd, pid_str, sizeof(pid_str)) != 0) {
        return port_buf;
    }

    long pid = atol(pid_str);
    if (pid <= 0) {
        return port_buf;
    }

    // Use ss instead of netstat (faster, modern)
    char ss_out[64] = {0};
    snprintf(cmd, sizeof(cmd),
             "ss -tlnp 2>/dev/null | awk '$0 ~ \"pid=%ld,?\" {split($4,a,\":\"); print a[length(a)]; exit}'",
             pid);

    if (execute_cmd(cmd, ss_out, sizeof(ss_out)) == 0 && strlen(ss_out) > 0) {
        int ok = 1;
        for (size_t i = 0; i < strlen(ss_out); i++) {
            if (!isdigit((unsigned char)ss_out[i])) {
                ok = 0;
                break;
            }
        }
        if (ok && strlen(ss_out) < sizeof(port_buf)) {
            strcpy(port_buf, ss_out);
        }
    }

    return port_buf;
}

// --------------------------------------------------
// Service-Summary - CACHED
// --------------------------------------------------

/* Returns cached summary if valid (TTL < 5s), otherwise queries systemd.
   Uses negative index scheme: for my_services use idx 0..N-1,
   for all_services during browse we use a separate cache path. */
void get_service_summary(const char *svc, char *summary, size_t bufsize) {
    // Check cache by matching against my_services
    for (int i = 0; i < num_my_services; i++) {
        if (strcmp(my_services[i], svc) == 0 && cache_valid[i]) {
            time_t now = time(NULL);
            if (now - cache_timestamp[i] < CACHE_TTL_SECONDS) {
                strncpy(summary, summary_cache[i], bufsize - 1);
                summary[bufsize - 1] = '\0';
                return; // cache hit
            }
        }
    }

    // Cache miss — query systemd
    char scope_str[16];
    strcpy(scope_str, detect_scope(svc));
    char active[32] = {0}, enabled[32] = {0}, desc[MAX_DESC] = {0}, port[16] = {0};
    char cmd[MAX_LINE];

    if (strcmp(scope_str, "system") == 0) {
        snprintf(cmd, sizeof(cmd), "systemctl is-active \"%s\" 2>/dev/null", svc);
        execute_cmd(cmd, active, sizeof(active));
        snprintf(cmd, sizeof(cmd), "systemctl is-enabled \"%s\" 2>/dev/null", svc);
        execute_cmd(cmd, enabled, sizeof(enabled));
        snprintf(cmd, sizeof(cmd), "systemctl show -p Description --value \"%s\" 2>/dev/null", svc);
        execute_cmd(cmd, desc, sizeof(desc));
    } else if (strcmp(scope_str, "user") == 0) {
        snprintf(cmd, sizeof(cmd), "systemctl --user is-active \"%s\" 2>/dev/null", svc);
        execute_cmd(cmd, active, sizeof(active));
        snprintf(cmd, sizeof(cmd), "systemctl --user is-enabled \"%s\" 2>/dev/null", svc);
        execute_cmd(cmd, enabled, sizeof(enabled));
        snprintf(cmd, sizeof(cmd), "systemctl --user show -p Description --value \"%s\" 2>/dev/null", svc);
        execute_cmd(cmd, desc, sizeof(desc));
    } else {
        strcpy(active, "not-found");
        strcpy(enabled, "not-found");
        strcpy(desc, "(Service nicht gefunden – weder system- noch user-weit)");
    }

    if (strlen(desc) == 0) strcpy(desc, "(keine Beschreibung)");
    strcpy(port, guess_port(svc, scope_str));

    snprintf(summary, bufsize, "%s|%s|%s|%s|%s",
             scope_str, active, enabled, desc, port);

    // Update cache for matching my_services entry
    for (int i = 0; i < num_my_services; i++) {
        if (strcmp(my_services[i], svc) == 0) {
            strncpy(summary_cache[i], summary, MAX_LINE - 1);
            summary_cache[i][MAX_LINE - 1] = '\0';
            cache_valid[i] = 1;
            cache_timestamp[i] = time(NULL);
            break;
        }
    }
}

/* Helper: safe snprintf for command building */
int execute_cmd_fmt(char *buf, size_t bufsize, const char *fmt, const char *arg) {
    int n = snprintf(buf, bufsize, fmt, arg);
    if (n < 0 || (size_t)n >= bufsize) return -1;
    return 0;
}

// --------------------------------------------------
// Alle Services einsammeln
// --------------------------------------------------
void build_all_services_list(const char *home) {
    num_all_services = 0;

    DIR *dir = opendir("/etc/systemd/system");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            size_t dlen = strlen(entry->d_name);
            if (dlen > 8 && strcmp(entry->d_name + dlen - 8, ".service") == 0) {
                strncpy(all_services[num_all_services], entry->d_name, MAX_LINE - 1);
                all_services[num_all_services][MAX_LINE - 1] = '\0';
                num_all_services++;
            }
        }
        closedir(dir);
    }

    // User services: also look for symlinks and directories (wants.requires)
    char search_dirs[3][MAX_LINE];
    snprintf(search_dirs[0], sizeof(search_dirs[0]), "%s/.config/systemd/user", home);
    snprintf(search_dirs[1], sizeof(search_dirs[1]), "%s/.config/systemd/user/*.wants", home);
    snprintf(search_dirs[2], sizeof(search_dirs[2]), "%s/.config/systemd/user/*.requires", home);

    for (int d = 0; d < 3; d++) {
        dir = opendir(search_dirs[d]);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            size_t dlen = strlen(entry->d_name);
            if (dlen > 8 && strcmp(entry->d_name + dlen - 8, ".service") == 0) {
                // Deduplicate
                int exists = 0;
                for (int i = 0; i < num_all_services; i++) {
                    if (strcmp(all_services[i], entry->d_name) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists) {
                    strncpy(all_services[num_all_services], entry->d_name, MAX_LINE - 1);
                    all_services[num_all_services][MAX_LINE - 1] = '\0';
                    num_all_services++;
                }
            }
        }
        closedir(dir);
    }
}

// --------------------------------------------------
// Alte CLI-Dialoge (legacy)
// --------------------------------------------------

static void print_header(const char *title) {
    system("clear");
    printf("%s=====================================================%s\n",
           HEADER_COLOR, RESET_COLOR);
    printf("%s        Systemd Dashboard – Eigene Services          %s\n",
           HEADER_COLOR, RESET_COLOR);
    printf("%s=====================================================%s\n",
           HEADER_COLOR, RESET_COLOR);
    if (title && strlen(title) > 0) {
        printf("%s%s%s\n", HEADER_COLOR, title, RESET_COLOR);
    }
    printf("\n");
}

void add_service_interactive(const char *home) {
    print_header("Service zu Favoriten hinzufuegen");
    printf("Gib den exakten Servicenamen ein (z.B. trainee_trainer-gunicorn.service):\n> ");

    char svc[MAX_LINE];
    if (!fgets(svc, sizeof(svc), stdin)) return;
    svc[strcspn(svc, "\n")] = '\0';

    if (svc[0] == '\0') {
        printf("%sAbgebrochen (kein Name).%s\n", DIM_COLOR, RESET_COLOR);
        press_enter_cli();
        return;
    }

    for (int i = 0; i < num_my_services; ++i) {
        if (strcmp(my_services[i], svc) == 0) {
            printf("%sService ist bereits in der Favoritenliste.%s\n",
                   WARN_COLOR, RESET_COLOR);
            press_enter_cli();
            return;
        }
    }

    char *scope = detect_scope(svc);
    if (strcmp(scope, "none") == 0) {
        printf("%sHinweis:%s Service wird aktuell weder als System- noch als User-Service gefunden.\n",
               WARN_COLOR, RESET_COLOR);
        printf("Trotzdem zu Favoriten hinzufuegen? [y/N] ");
        char ans[8];
        if (!fgets(ans, sizeof(ans), stdin)) return;
        if (tolower((unsigned char)ans[0]) != 'y') {
            printf("%sNicht hinzugefuegt.%s\n", DIM_COLOR, RESET_COLOR);
            press_enter_cli();
            return;
        }
    } else {
        printf("%sErkannt als %s-Service.%s\n", OK_COLOR, scope, RESET_COLOR);
    }

    if (num_my_services < MAX_SERVICES) {
        strncpy(my_services[num_my_services], svc, MAX_LINE - 1);
        my_services[num_my_services][MAX_LINE - 1] = '\0';
        num_my_services++;
        save_services(home);
        printf("%sService zu Favoriten hinzugefuegt.%s\n", OK_COLOR, RESET_COLOR);
    } else {
        printf("%sMaximale Anzahl Services erreicht.%s\n", ERR_COLOR, RESET_COLOR);
    }
    press_enter_cli();
}

void remove_service_interactive(const char *home) {
    print_header("Service aus Favoriten entfernen");

    if (num_my_services == 0) {
        printf("%sKeine Favoriten vorhanden.%s\n", DIM_COLOR, RESET_COLOR);
        press_enter_cli();
        return;
    }

    for (int i = 0; i < num_my_services; ++i) {
        printf("  %d) %s\n", i + 1, my_services[i]);
    }

    printf("\nNummer zum Entfernen (oder leer fuer Abbruch): ");
    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) return;
    buf[strcspn(buf, "\n")] = '\0';
    if (buf[0] == '\0') {
        printf("%sAbgebrochen.%s\n", DIM_COLOR, RESET_COLOR);
        press_enter_cli();
        return;
    }

    int idx = atoi(buf);
    if (idx < 1 || idx > num_my_services) {
        printf("%sUngueltige Auswahl.%s\n", ERR_COLOR, RESET_COLOR);
        press_enter_cli();
        return;
    }

    idx--; // 0-based
    char removed[MAX_LINE];
    strncpy(removed, my_services[idx], MAX_LINE - 1);
    removed[MAX_LINE - 1] = '\0';
    invalidate_service_cache(my_services[idx]);

    for (int i = idx; i < num_my_services - 1; ++i) {
        memmove(my_services[i], my_services[i + 1], MAX_LINE);
    }
    num_my_services--;
    save_services(home);

    printf("%sService entfernt:%s %s\n", OK_COLOR, RESET_COLOR, removed);
    press_enter_cli();
}

// --------------------------------------------------
// Main Loop - with refresh throttle and resize handling
// --------------------------------------------------
void main_loop(const char *home) {
    init_ui();

    // Register SIGWINCH handler
    signal(SIGWINCH, resize_handler);

    int selected = 0;
    int focus_on_list = 1;
    int needs_render = 1; // initial render
    int render_count = 0; // throttle counter

    render_dashboard_ui(selected, focus_on_list);

    while (1) {
        // Handle terminal resize
        if (need_resize) {
            need_resize = 0;
            endwin();
            refresh();
            doupdate();
            needs_render = 1;
        }

        // Bounds checks
        if (num_my_services <= 0) {
            selected = 0;
        } else if (selected >= num_my_services) {
            selected = num_my_services - 1;
        }

        // Throttle: only render every 10th tick if nothing changed
        render_count++;
        if (render_count >= 10) {
            render_count = 0;
            needs_render = 1;
        }

        int ch = getch();

        if (ch == -1 || ch == ERR) {
            // No input - only render if throttle triggered
            if (needs_render) {
                render_dashboard_ui(selected, focus_on_list);
                needs_render = 0;
            }
            continue;
        }

        // Key handling
        if (ch == 'q' || ch == 'Q') {
            end_ui();
            printf("\n%sBye%s\n", DIM_COLOR, RESET_COLOR);
            exit(0);
        } else if (ch == '\t') {
            focus_on_list = !focus_on_list;
            needs_render = 1;
        } else if (focus_on_list && num_my_services > 0 && (ch == KEY_UP || ch == 'k')) {
            selected = (selected > 0) ? selected - 1 : num_my_services - 1;
            needs_render = 1;
        } else if (focus_on_list && num_my_services > 0 && (ch == KEY_DOWN || ch == 'j')) {
            selected = (selected < num_my_services - 1) ? selected + 1 : 0;
            needs_render = 1;
        } else if ((ch == '\n' || ch == KEY_ENTER) && focus_on_list && num_my_services > 0) {
            service_detail_page_ui(my_services[selected]);
            needs_render = 1;
            render_count = 0;
        } else if (ch == 'a' || ch == 'A') {
            add_service_ui(home);
            invalidate_cache();
            needs_render = 1;
        } else if (ch == 'r') {
            remove_service_ui(home);
            invalidate_cache();
            needs_render = 1;
        } else if (ch == 'R') {
            load_services(home);
            needs_render = 1;
        } else if (ch == 'B' || ch == 'b') {
            browse_all_services_ui(home);
            invalidate_cache(); // cache may be stale after browse
            needs_render = 1;
        } else if (ch == 'o' || ch == 'O') {
            if (focus_on_list && num_my_services > 0) {
                const char *svc = my_services[selected];
                char *scope = detect_scope(svc);
                char *port  = guess_port(svc, scope);
                if (port && strcmp(port, "-") != 0 && strlen(port) > 0) {
                    open_in_browser_ui(port);
                } else {
                    show_message_ui("Kein Port erkannt oder Service lauscht nicht.");
                }
            } else {
                show_message_ui("Kein Service ausgewaehlt.");
            }
        }

        if (needs_render) {
            render_dashboard_ui(selected, focus_on_list);
            needs_render = 0;
            render_count = 0;
        }
    }
}

// --------------------------------------------------
// main
// --------------------------------------------------
int main(void) {
    check_systemctl();
    init_sudo_flag();

    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME nicht gesetzt\n");
        return 1;
    }

    load_services(home);
    main_loop(home);
    end_ui();

    printf("\n%sBye%s\n", DIM_COLOR, RESET_COLOR);
    return 0;
}
