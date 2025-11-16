#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>

#include "sys_dashboard.h"
#include "utils.h"

// Browser öffnen – simpel: localhost:PORT
void open_in_browser(const char *port_str) {
    if (!port_str || port_str[0] == '\0' || strcmp(port_str, "-") == 0) {
        printf("%sKein gültiger Port zum Öffnen.%s\n", WARN_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "xdg-open \"http://localhost:%s\" >/dev/null 2>&1 &",
             port_str);
    system(cmd);
}

// Sehr einfache Ressourcenabfrage über /proc/<pid>/statm
// CPU wird hier nicht wirklich berechnet → 0.0f
int get_resource_usage(long pid, float *cpu_pct, long *rss_kb) {
    if (pid <= 0 || !cpu_pct || !rss_kb) return -1;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/statm", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    long total_pages = 0;
    long rss_pages   = 0;
    if (fscanf(fp, "%ld %ld", &total_pages, &rss_pages) != 2) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    *rss_kb  = rss_pages * page_kb;
    *cpu_pct = 0.0f;  // für eine echte CPU-Berechnung bräuchte man Zeitdeltas

    return 0;
}

// Dependencies zeigen – wir springen kurz aus ncurses raus
void show_dependencies(const char *svc, const char *scope) {
    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");
    char cmd[512];

    def_prog_mode();
    endwin();

    printf("%sAbhängigkeiten von %s%s\n\n", HEADER_COLOR, svc, RESET_COLOR);
    snprintf(cmd, sizeof(cmd),
             "systemctl %s list-dependencies \"%s\"",
             user_flag, svc);
    system(cmd);

    printf("\n%sDrücke Enter für Zurück...%s", DIM_COLOR, RESET_COLOR);
    press_enter();

    reset_prog_mode();
    refresh();
}

void press_enter(void) {
    char dummy[8];
    fgets(dummy, sizeof(dummy), stdin);
}