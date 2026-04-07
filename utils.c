#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include <sys/stat.h>

#include "sys_dashboard.h"
#include "utils.h"
#include "ui.h"

// Externe Deklarationen
extern int execute_cmd(const char *cmd, char *output, size_t max_output);
extern void show_message_ui(const char *msg);
extern void invalidate_cache(void);
extern void press_enter(void);
extern void load_services(const char *home);

// Color-Defines (müssen mit sys_dashboard.c übereinstimmen)
#define OK_COLOR "\033[32m"
#define WARN_COLOR "\033[33m"
#define ERR_COLOR "\033[31m"
#define HEADER_COLOR "\033[1;36m"
#define DIM_COLOR "\033[2m"
#define RESET_COLOR "\033[0m"

// --------------------------------------------------
// Browser öffnen (ncurses-safe)
// --------------------------------------------------
void open_in_browser_ui(const char *port_str) {
    if (!port_str || port_str[0] == '\0' || strcmp(port_str, "-") == 0) {
        show_message_ui("Kein gueltiger Port zum Oeffnen.");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "xdg-open \"http://localhost:%s\" >/dev/null 2>&1 &",
             port_str);
    system(cmd);
    show_message_ui("Browser geoeffnet: http://localhost:");
}

// --------------------------------------------------
// Ressourcenabfrage über /proc/<pid>/
// --------------------------------------------------
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
    *cpu_pct = 0.0f;

    return 0;
}

// --------------------------------------------------
// Dependencies anzeigen (ncurses-safe)
// --------------------------------------------------
void show_dependencies_ui(const char *svc, const char *scope) {
    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");
    char cmd[512];

    def_prog_mode();
    endwin();

    printf("%sAbhaengigkeiten von %s%s\n\n", HEADER_COLOR, svc, RESET_COLOR);
    snprintf(cmd, sizeof(cmd),
             "systemctl %s list-dependencies \"%s\"",
             user_flag, svc);
    system(cmd);

    printf("\n%sDruecke Enter fuer Zurueck...%s", DIM_COLOR, RESET_COLOR);

    // Robuster: auf vollen Enter warten
    char dummy[64];
    if (fgets(dummy, sizeof(dummy), stdin) == NULL) {
        getchar(); // fallback
    }

    reset_prog_mode();
    refresh();
}

// --------------------------------------------------
// Unit-File bearbeiten + auto daemon-reload
// --------------------------------------------------
void edit_unit_file_ui(const char *svc, const char *scope) {
    if (!svc || strcmp(svc, "") == 0) {
        show_message_ui("Kein gueltiger Service-Name.");
        return;
    }

    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");
    char fragment_path[MAX_LINE] = {0};
    char cmd[MAX_LINE];

    // Pfad zur Unit-File holen
    snprintf(cmd, sizeof(cmd),
             "systemctl %s show -p FragmentPath --value \"%s\" 2>/dev/null",
             user_flag, svc);
    if (execute_cmd(cmd, fragment_path, sizeof(fragment_path)) != 0 ||
        strlen(fragment_path) == 0) {
        show_message_ui("Unit-File-Pfad nicht gefunden.");
        return;
    }

    // Prüfen ob Datei existiert und lesbar ist
    if (access(fragment_path, F_OK) != 0) {
        show_message_ui("Unit-File existiert nicht (möglicherweise generiert).");
        return;
    }

    def_prog_mode();
    endwin();

    printf("%sOeffne Unit-File: %s%s\n", HEADER_COLOR, fragment_path, RESET_COLOR);
    printf("%sBearbeite mit nano (oder vi als Fallback).%s\n", WARN_COLOR, RESET_COLOR);
    printf("%sNach dem Speichern wird automatisch daemon-reload ausgeführt.%s\n\n",
           OK_COLOR, RESET_COLOR);

    // Editor öffnen (nano, dann vi)
    char editor_cmd[512];
    snprintf(editor_cmd, sizeof(editor_cmd),
             "if command -v nano >/dev/null 2>&1; then nano \"%s\"; else vi \"%s\"; fi",
             fragment_path, fragment_path);
    int ret = system(editor_cmd);

    if (WIFEXITED(ret)) {
        int exit_code = WEXITSTATUS(ret);
        if (exit_code == 0) {
            // Erfolgsfall: daemon-reload ausführen
            printf("\n%sFühre daemon-reload aus...%s\n", OK_COLOR, RESET_COLOR);
            char reload_cmd[MAX_LINE];
            snprintf(reload_cmd, sizeof(reload_cmd),
                     "%ssystemctl %s daemon-reload 2>/dev/null",
                     (geteuid() == 0 ? "" : "sudo "), user_flag);
            int reload_ret = system(reload_cmd);

            if (WIFEXITED(reload_ret) && WEXITSTATUS(reload_ret) == 0) {
                printf("%sdaemon-reload erfolgreich.%s\n", OK_COLOR, RESET_COLOR);
            } else {
                printf("%sdaemon-reload fehlgeschlagen (Exit %d).%s\n",
                       WARN_COLOR, WIFEXITED(reload_ret) ? WEXITSTATUS(reload_ret) : -1, RESET_COLOR);
            }
        } else {
            printf("\n%sEditor mit Exit-Code %d beendet.%s\n",
                   WARN_COLOR, exit_code, RESET_COLOR);
        }
    }

    printf("\n%sDruecke Enter fuer Zurueck...%s", DIM_COLOR, RESET_COLOR);
    char dummy[64];
    if (fgets(dummy, sizeof(dummy), stdin) == NULL) {
        getchar();
    }

    reset_prog_mode();
    refresh();
}

// --------------------------------------------------
// Legacy press_enter (CLI-only)
// --------------------------------------------------
void press_enter(void) {
    char dummy[64];
    // Robuster: versuche fgets, fallback getchar
    if (fgets(dummy, sizeof(dummy), stdin) == NULL) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF);
    }
}
