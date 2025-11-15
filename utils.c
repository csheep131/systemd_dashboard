// utils.c – Sonstige Utilities für das Dashboard

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <ncurses.h>

#include "sys_dashboard.h"
#include "utils.h"

// ---------------------------------------------------------
// Port im Browser öffnen (localhost:PORT)
// ---------------------------------------------------------
void open_in_browser(const char *port_str) {
    if (!port_str || port_str[0] == '\0' || port_str[0] == '-')
        return;

    // Nur Ziffern erlauben
    for (const char *p = port_str; *p; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return;
        }
    }

    char url[128];
    snprintf(url, sizeof(url), "http://localhost:%s", port_str);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", url);
    system(cmd);
}

// ---------------------------------------------------------
// CPU/RAM über ps
// ---------------------------------------------------------
int get_resource_usage(long pid, float *cpu_pct, long *rss_kb) {
    if (pid <= 0 || !cpu_pct || !rss_kb) return -1;

    char cmd[MAX_LINE];
    char out[MAX_LINE];

    snprintf(cmd, sizeof(cmd),
             "ps -p %ld -o %%cpu=,rss= 2>/dev/null", pid);

    if (execute_cmd(cmd, out, sizeof(out)) != 0 || strlen(out) == 0)
        return -1;

    float cpu_local = 0.0f;
    long  rss_local = 0;
    if (sscanf(out, "%f %ld", &cpu_local, &rss_local) != 2)
        return -1;

    *cpu_pct = cpu_local;
    *rss_kb  = rss_local;
    return 0;
}

// ---------------------------------------------------------
// Dependencies mit systemctl list-dependencies
// ---------------------------------------------------------
void show_dependencies(const char *svc, const char *scope) {
    if (!svc || !scope) return;

    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");

    char cmd[MAX_LINE * 2];
    snprintf(cmd, sizeof(cmd),
             "systemctl %s list-dependencies \"%s\" | less",
             user_flag, svc);

    // ncurses kurz verlassen
    def_prog_mode();
    endwin();

    system(cmd);

    // zurück zu ncurses
    reset_prog_mode();
    refresh();
}
