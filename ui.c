#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ui.h"
#include "sys_dashboard.h"
#include "utils.h"

static WINDOW *main_win = NULL;
static WINDOW *status_win = NULL;

// Farb-Paare
// 1: Header (cyan)
// 2: OK / "active"
// 3: Warn / "activating"/"static"/"indirect"
// 4: Fehler / "failed"
// 5: Standard Text
// 6: "inactive" / dim
static void init_colors(void) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN,   -1);
    init_pair(2, COLOR_GREEN,  -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_RED,    -1);
    init_pair(5, COLOR_WHITE,  -1);
    init_pair(6, COLOR_BLACK,  -1);  // "grau" imitieren mit dunklem Text
}

void init_ui(void) {
    initscr();
    if (!has_colors()) {
        endwin();
        printf("Terminal unterstützt keine Farben.\n");
        exit(1);
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    init_colors();
    refresh();

    int h = LINES;
    int w = COLS;

    main_win   = newwin(h - 3, w, 0, 0);
    status_win = newwin(3, w, h - 3, 0);
    scrollok(main_win, TRUE);
}

void end_ui(void) {
    if (main_win) {
        delwin(main_win);
        main_win = NULL;
    }
    if (status_win) {
        delwin(status_win);
        status_win = NULL;
    }
    endwin();
}

void show_message_ui(const char *msg) {
    werase(status_win);
    wattron(status_win, COLOR_PAIR(4));
    mvwprintw(status_win, 1, 1, "%s (Taste drücken)", msg);
    wattroff(status_win, COLOR_PAIR(4));
    wrefresh(status_win);
    wgetch(status_win);
    werase(status_win);
    wrefresh(status_win);
}

static int color_for_status(const char *s) {
    if (!s || !*s) return 5;
    if (strcmp(s, "active") == 0)      return 2;
    if (strcmp(s, "failed") == 0)      return 4;
    if (strcmp(s, "inactive") == 0)    return 6;
    if (strcmp(s, "activating") == 0 ||
        strcmp(s, "deactivating") == 0) return 3;
    if (strcmp(s, "not-found") == 0)   return 4;
    return 3;
}

static int color_for_enabled(const char *s) {
    if (!s || !*s) return 5;
    if (strcmp(s, "enabled") == 0)  return 2;
    if (strcmp(s, "disabled") == 0) return 6;
    if (strcmp(s, "static") == 0 ||
        strcmp(s, "indirect") == 0 ||
        strcmp(s, "generated") == 0) return 3;
    if (strcmp(s, "not-found") == 0) return 4;
    return 3;
}

// --------------------------------------------------
// Dashboard-Rendering
// --------------------------------------------------

void render_dashboard_ui(int selected_idx, int focus_on_list) {
    (void)focus_on_list; // aktuell nicht genutzt, aber für später vorbereitbar

    werase(main_win);
    int y = 0;

    // Header
    wattron(main_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(main_win, y++, 0, "==============================================================");
    mvwprintw(main_win, y++, 0, "        Systemd Dashboard Eigene Services (Favoriten)        ");
    mvwprintw(main_win, y++, 0, "==============================================================");
    wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
    y++;

    if (num_my_services == 0) {
        wattron(main_win, COLOR_PAIR(4));
        mvwprintw(main_win, y++, 0, "Keine Services konfiguriert.");
        wattroff(main_win, COLOR_PAIR(4));
        mvwprintw(main_win, y++, 0, "Mit 'a' kannst du Services hinzufügen.");
    } else {
        mvwprintw(main_win, y++, 0,
                  "Nr  SCOPE SERVICE                         ACTIVE      ENABLED     PORT   DESCRIPTION");
        mvwprintw(main_win, y++, 0,
                  "---- ----- ------------------------------ ---------- ---------- ------ ---------------------------");

        for (int i = 0; i < num_my_services && y < LINES - 4; ++i) {
            char summary[MAX_LINE];
            get_service_summary(my_services[i], summary, sizeof(summary));

            char temp[MAX_LINE];
            strncpy(temp, summary, sizeof(temp) - 1);
            temp[sizeof(temp)-1] = '\0';

            char parts[5][MAX_DESC] = {{0}};
            char *tok = strtok(temp, "|");
            int j = 0;
            while (tok && j < 5) {
                strncpy(parts[j], tok, MAX_DESC - 1);
                parts[j][MAX_DESC - 1] = '\0';
                tok = strtok(NULL, "|");
                j++;
            }
            if (j < 5) continue;

            const char *scope_raw = parts[0];
            const char *active    = parts[1];
            const char *enabled   = parts[2];
            const char *desc      = parts[3];
            const char *port      = parts[4];

            char scope_disp[8];
            if (strcmp(scope_raw, "system") == 0) strcpy(scope_disp, "SYS");
            else if (strcmp(scope_raw, "user") == 0) strcpy(scope_disp, "USR");
            else strcpy(scope_disp, "???");

            if (i == selected_idx) wattron(main_win, A_REVERSE);

            // Nummer, Scope, Service
            mvwprintw(main_win, y, 0,  "%-3d ", i + 1);
            mvwprintw(main_win, y, 4,  "%-4s", scope_disp);
            mvwprintw(main_win, y, 9,  "%-30.30s", my_services[i]);

            // ACTIVE in Farbe
            int c_status = color_for_status(active);
            wattron(main_win, COLOR_PAIR(c_status));
            mvwprintw(main_win, y, 40, "%-10.10s", active);
            wattroff(main_win, COLOR_PAIR(c_status));

            // ENABLED in Farbe
            int c_enabled = color_for_enabled(enabled);
            wattron(main_win, COLOR_PAIR(c_enabled));
            mvwprintw(main_win, y, 51, "%-10.10s", enabled);
            wattroff(main_win, COLOR_PAIR(c_enabled));

            // Port + Description
            mvwprintw(main_win, y, 62, "%-6.6s", port);
            mvwprintw(main_win, y, 69, "%-40.40s", desc);

            if (i == selected_idx) wattroff(main_win, A_REVERSE);
            y++;
        }
    }

    // Statuszeile unten
    werase(status_win);
    wattron(status_win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(status_win, 0, 0,
              "Pfeile hoch runter: Auswahl  Enter: Details  a: Add  r: Remove  R: Reload  B: Browse All  q: Quit");
    mvwprintw(status_win, 1, 0,
              "Hinweis: Farben grün=active/enabled, rot=failed, gelb=aktivierend, dunkel=inaktiv");
    wattroff(status_win, COLOR_PAIR(5) | A_BOLD);

    wrefresh(main_win);
    wrefresh(status_win);
}

// --------------------------------------------------
// Browse All Services UI
// --------------------------------------------------

void browse_all_services_ui(const char *home) {
    build_all_services_list(home);

    int selected = 0;
    char filter[256] = "";
    int  filtered_idx[MAX_SERVICES];
    int  filtered_count = 0;

    while (1) {
        werase(main_win);
        int y = 0;

        wattron(main_win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(main_win, y++, 0, "==============================================================");
        mvwprintw(main_win, y++, 0, "        Alle Services – System + User                         ");
        mvwprintw(main_win, y++, 0, "==============================================================");
        wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
        y++;

        mvwprintw(main_win, y++, 0,
                  "Suche: [%s]  (/ zum Editieren, ↑/↓, Enter: Details, a: Add Fav, b: Zurück)",
                  filter);
        y++;

        filtered_count = 0;
        for (int i = 0; i < num_all_services; ++i) {
            if (filter[0] &&
                strstr(all_services[i], filter) == NULL) {
                continue;
            }
            if (filtered_count < MAX_SERVICES) {
                filtered_idx[filtered_count++] = i;
            }
        }

        mvwprintw(main_win, y++, 0,
                  "Nr  SCOPE SERVICE                         ACTIVE      ENABLED     PORT   DESCRIPTION");
        mvwprintw(main_win, y++, 0,
                  "---- ----- ------------------------------ ---------- ---------- ------ ---------------------------");

        for (int k = 0; k < filtered_count && y < LINES - 4; ++k) {
            int idx = filtered_idx[k];

            char summary[MAX_LINE];
            get_service_summary(all_services[idx], summary, sizeof(summary));

            char temp[MAX_LINE];
            strncpy(temp, summary, sizeof(temp) - 1);
            temp[sizeof(temp)-1] = '\0';

            char parts[5][MAX_DESC] = {{0}};
            char *tok = strtok(temp, "|");
            int j = 0;
            while (tok && j < 5) {
                strncpy(parts[j], tok, MAX_DESC - 1);
                parts[j][MAX_DESC - 1] = '\0';
                tok = strtok(NULL, "|");
                j++;
            }
            if (j < 5) continue;

            const char *scope_raw = parts[0];
            const char *active    = parts[1];
            const char *enabled   = parts[2];
            const char *desc      = parts[3];
            const char *port      = parts[4];

            char scope_disp[8];
            if (strcmp(scope_raw, "system") == 0) strcpy(scope_disp, "SYS");
            else if (strcmp(scope_raw, "user") == 0) strcpy(scope_disp, "USR");
            else strcpy(scope_disp, "???");

            if (k == selected) wattron(main_win, A_REVERSE);

            mvwprintw(main_win, y, 0,  "%-3d ", k + 1);
            mvwprintw(main_win, y, 4,  "%-4s", scope_disp);
            mvwprintw(main_win, y, 9,  "%-30.30s", all_services[idx]);

            int c_status  = color_for_status(active);
            int c_enabled = color_for_enabled(enabled);

            wattron(main_win, COLOR_PAIR(c_status));
            mvwprintw(main_win, y, 40, "%-10.10s", active);
            wattroff(main_win, COLOR_PAIR(c_status));

            wattron(main_win, COLOR_PAIR(c_enabled));
            mvwprintw(main_win, y, 51, "%-10.10s", enabled);
            wattroff(main_win, COLOR_PAIR(c_enabled));

            mvwprintw(main_win, y, 62, "%-6.6s", port);
            mvwprintw(main_win, y, 69, "%-40.40s", desc);

            if (k == selected) wattroff(main_win, A_REVERSE);
            y++;
        }

        werase(status_win);
        wattron(status_win, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(status_win, 0, 0,
                  "Browse All: Pfeil Auswahl  Enter Details  a Add zu Favoriten  / Filter  b Zurück");
        wattroff(status_win, COLOR_PAIR(5) | A_BOLD);

        wrefresh(main_win);
        wrefresh(status_win);

        int ch = getch();
        if (ch == KEY_UP) {
            if (selected > 0) selected--;
        } else if (ch == KEY_DOWN) {
            if (selected < filtered_count - 1) selected++;
        } else if (ch == '/') {
            echo();
            mvwprintw(status_win, 1, 0, "Filter: ");
            wclrtoeol(status_win);
            wrefresh(status_win);
            wgetnstr(status_win, filter, sizeof(filter) - 1);
            noecho();
            selected = 0;
        } else if ((ch == 10 || ch == KEY_ENTER) && filtered_count > 0) {
            int idx = filtered_idx[selected];
            service_detail_page_ui(all_services[idx]);
        } else if ((ch == 'b') || (ch == 'B') || ch == 27) {
            break;
        } else if (ch == 'a' || ch == 'A') {
            if (filtered_count > 0) {
                int idx = filtered_idx[selected];
                const char *svc = all_services[idx];

                // schon in Favoriten?
                int exists = 0;
                for (int i = 0; i < num_my_services; ++i) {
                    if (strcmp(my_services[i], svc) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (exists) {
                    show_message_ui("Service bereits in Favoriten.");
                } else if (num_my_services >= MAX_SERVICES) {
                    show_message_ui("Maximale Anzahl Favoriten erreicht.");
                } else {
                    strncpy(my_services[num_my_services++], svc, MAX_LINE - 1);
                    my_services[num_my_services-1][MAX_LINE-1] = '\0';
                    save_services(home);
                    show_message_ui("Service zu Favoriten hinzugefügt.");
                }
            }
        }
    }
}

// --------------------------------------------------
// Detailseite
// --------------------------------------------------

void service_detail_page_ui(const char *svc) {
    while (1) {
        werase(main_win);
        int y = 0;

        wattron(main_win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(main_win, y++, 0, "==============================================================");
        mvwprintw(main_win, y++, 0, "Service-Details: %s", svc);
        mvwprintw(main_win, y++, 0, "==============================================================");
        wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
        y++;

        char summary[MAX_LINE];
        get_service_summary(svc, summary, sizeof(summary));

        char temp[MAX_LINE];
        strncpy(temp, summary, sizeof(temp) - 1);
        temp[sizeof(temp)-1] = '\0';

        char parts[5][MAX_DESC] = {{0}};
        char *tok = strtok(temp, "|");
        int j = 0;
        while (tok && j < 5) {
            strncpy(parts[j], tok, MAX_DESC - 1);
            parts[j][MAX_DESC - 1] = '\0';
            tok = strtok(NULL, "|");
            j++;
        }
        if (j < 5) {
            show_message_ui("Fehler beim Laden der Summary.");
            return;
        }

        const char *scope_raw = parts[0];
        const char *active    = parts[1];
        const char *enabled   = parts[2];
        const char *desc      = parts[3];
        const char *port      = parts[4];

        char scope_label[64];
        if (strcmp(scope_raw, "system") == 0)
            strcpy(scope_label, "System-Service");
        else if (strcmp(scope_raw, "user") == 0)
            strcpy(scope_label, "User-Service (~/.config/systemd/user)");
        else
            strcpy(scope_label, "NICHT GEFUNDEN");

        if (strcmp(scope_raw, "none") == 0) {
            wattron(main_win, COLOR_PAIR(4));
            mvwprintw(main_win, y++, 0,
                      "ACHTUNG: Service wird weder als System- noch als User-Service gefunden.");
            wattroff(main_win, COLOR_PAIR(4));
            y++;
        }

        // Übersicht
        wattron(main_win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(main_win, y++, 0, "Übersicht");
        wattroff(main_win, COLOR_PAIR(1) | A_BOLD);

        mvwprintw(main_win, y++, 0, "  Name:        %s", svc);
        mvwprintw(main_win, y++, 0, "  Scope:       %s", scope_label);

        int c_status  = color_for_status(active);
        int c_enabled = color_for_enabled(enabled);

        mvwprintw(main_win, y, 0, "  Active:      ");
        wattron(main_win, COLOR_PAIR(c_status));
        wprintw(main_win, "%s", active);
        wattroff(main_win, COLOR_PAIR(c_status));
        y++;

        mvwprintw(main_win, y, 0, "  Enabled:     ");
        wattron(main_win, COLOR_PAIR(c_enabled));
        wprintw(main_win, "%s", enabled);
        wattroff(main_win, COLOR_PAIR(c_enabled));
        y++;

        mvwprintw(main_win, y++, 0, "  Port:        %s", port);
        mvwprintw(main_win, y++, 0, "  Description: %s", desc);

        // Zusätzliche Infos
        char act_ts[MAX_LINE]       = {0};
        char inact_ts[MAX_LINE]     = {0};
        char fragment_path[MAX_LINE]= {0};
        char unit_state[32]         = {0};
        char substate[32]           = {0};
        char cmd[MAX_LINE];

        const char *user_flag = (strcmp(scope_raw, "system") == 0 ? "" : "--user");

        if (strcmp(scope_raw, "none") != 0) {
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p ActiveEnterTimestamp --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, act_ts, sizeof(act_ts));

            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p InactiveEnterTimestamp --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, inact_ts, sizeof(inact_ts));

            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p FragmentPath --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, fragment_path, sizeof(fragment_path));

            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p LoadState --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, unit_state, sizeof(unit_state));

            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p SubState --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, substate, sizeof(substate));
        }

        wattron(main_win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(main_win, y++, 0, "Zusätzliche Infos");
        wattroff(main_win, COLOR_PAIR(1) | A_BOLD);

        mvwprintw(main_win, y++, 0, "  LoadState:     %s", strlen(unit_state) ? unit_state : "unknown");
        mvwprintw(main_win, y++, 0, "  SubState:      %s", strlen(substate) ? substate : "unknown");
        mvwprintw(main_win, y++, 0, "  Fragment:      %s", strlen(fragment_path) ? fragment_path : "unknown");
        mvwprintw(main_win, y++, 0, "  Active seit:   %s", strlen(act_ts) ? act_ts : "n/a");
        mvwprintw(main_win, y++, 0, "  Inactive seit: %s", strlen(inact_ts) ? inact_ts : "n/a");

        // Logs (kurze Vorschau)
        wattron(main_win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(main_win, y++, 0, "Letzte Logs (journalctl -u %s -n %d)", svc, DETAIL_LOG_LINES);
        wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
        y++;

        snprintf(cmd, sizeof(cmd),
                 "journalctl %s -u \"%s\" -n %d --no-pager 2>/dev/null",
                 user_flag, svc, DETAIL_LOG_LINES);

        FILE *log_fp = popen(cmd, "r");
        if (log_fp) {
            char log_line[MAX_LINE];
            while (fgets(log_line, sizeof(log_line), log_fp) &&
                   y < LINES - 4) {
                log_line[strcspn(log_line, "\n")] = '\0';
                mvwprintw(main_win, y++, 0, "%s", log_line);
            }
            pclose(log_fp);
        } else {
            mvwprintw(main_win, y++, 0, "(Keine Logs verfügbar)");
        }

        werase(status_win);
        wattron(status_win, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(status_win, 0, 0,
          "s Start | t Stop | r Restart | e Enable | d Disable | S Status | I Show | L Live-Logs");
        mvwprintw(status_win, 1, 0,
          "o Browser | c CPU/RAM | D Deps | b Zurück");
        wattroff(status_win, COLOR_PAIR(5) | A_BOLD);

        wrefresh(main_win);
        wrefresh(status_win);

        extern const char *sudo_flag; // aus sys_dashboard.c
        int ch = getch();

        if (ch == 'b' || ch == 'B' || ch == 27) {
            break;
        } else if (ch == 's') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s start \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Service gestartet.");
        } else if (ch == 't' || ch == 'T') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s stop \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Service gestoppt.");
        } else if (ch == 'r' || ch == 'R') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s restart \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Service neugestartet.");
        } else if (ch == 'e' || ch == 'E') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s enable \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Service enabled.");
        } else if (ch == 'd') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s disable \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Service disabled.");
        } else if (ch == 'S') {
            def_prog_mode();
            endwin();
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s status \"%s\" | less",
                     user_flag, svc);
            system(cmd);
            reset_prog_mode();
            refresh();
        } else if (ch == 'I') {
            def_prog_mode();
            endwin();
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show \"%s\" | less",
                     user_flag, svc);
            system(cmd);
            reset_prog_mode();
            refresh();
        } else if (ch == 'L') {
            def_prog_mode();
            endwin();
            snprintf(cmd, sizeof(cmd),
                     "journalctl %s -u \"%s\" -f",
                     user_flag, svc);
            system(cmd);
            reset_prog_mode();
            refresh();
        } else if (ch == 'o' || ch == 'O') {
            open_in_browser(port);
        } else if (ch == 'c' || ch == 'C') {
            char pid_str[32];
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p MainPID --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, pid_str, sizeof(pid_str));
            long pid = atol(pid_str);
            float cpu = 0.0f;
            long  rss = 0;
            char msg[128];

            if (pid > 0 && get_resource_usage(pid, &cpu, &rss) == 0) {
                snprintf(msg, sizeof(msg),
                         "PID: %ld | CPU: %.1f%% | RAM: %ld KB",
                         pid, cpu, rss);
            } else {
                snprintf(msg, sizeof(msg), "Ressourceninfo nicht verfügbar.");
            }
            show_message_ui(msg);
        } else if (ch == 'D') {
            show_dependencies(svc, scope_raw);
        } else {
            show_message_ui("Unbekannte Aktion.");
        }
    }
}

// --------------------------------------------------
// Wrapper für Add/Remove (CLI-Dialoge)
// --------------------------------------------------

void add_service_ui(const char *home) {
    def_prog_mode();
    endwin();
    add_service_interactive(home);
    reset_prog_mode();
    refresh();
}

void remove_service_ui(const char *home) {
    def_prog_mode();
    endwin();
    remove_service_interactive(home);
    reset_prog_mode();
    refresh();
}
