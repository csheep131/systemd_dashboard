#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "sys_dashboard.h"
#include "ui.h"
#include "utils.h"

static WINDOW *main_win = NULL;
static WINDOW *status_win = NULL;

// ---------------------------------------------------------
// Farben
// ---------------------------------------------------------
static void init_colors(void) {
    start_color();
    init_pair(1, COLOR_CYAN,   COLOR_BLACK);  // Header
    init_pair(2, COLOR_GREEN,  COLOR_BLACK);  // OK
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);  // Warn
    init_pair(4, COLOR_RED,    COLOR_BLACK);  // Fehler
    init_pair(5, COLOR_WHITE,  COLOR_BLACK);  // Statuszeile
}

// ---------------------------------------------------------
// UI init / end
// ---------------------------------------------------------
void init_ui(void) {
    initscr();
    if (!has_colors()) {
        endwin();
        fprintf(stderr, "Terminal unterstützt keine Farben.\n");
        exit(1);
    }
    init_colors();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    refresh();

    int rows = LINES;
    int cols = COLS;

    main_win   = newwin(rows - 3, cols, 0, 0);
    status_win = newwin(3, cols, rows - 3, 0);

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

// ---------------------------------------------------------
// Statusmeldung unten
// ---------------------------------------------------------
void show_message_ui(const char *msg) {
    werase(status_win);
    wattron(status_win, COLOR_PAIR(4));
    mvwprintw(status_win, 1, 0, "%s – Enter zum Fortfahren", msg);
    wattroff(status_win, COLOR_PAIR(4));
    wrefresh(status_win);
    wgetch(status_win);
    werase(status_win);
    wrefresh(status_win);
}

// ---------------------------------------------------------
// Dashboard rendern (Favoriten)
// ---------------------------------------------------------
void render_dashboard_ui(void) {
    werase(main_win);
    int y = 0;

    wattron(main_win, COLOR_PAIR(1));
    mvwprintw(main_win, y++, 0, "=====================================================");
    mvwprintw(main_win, y++, 0, "        Systemd Dashboard – Eigene Services          ");
    mvwprintw(main_win, y++, 0, "=====================================================");
    wattroff(main_win, COLOR_PAIR(1));
    y++;

    if (num_my_services == 0) {
        wattron(main_win, COLOR_PAIR(4));
        mvwprintw(main_win, y++, 0, "Keine Services konfiguriert.");
        wattroff(main_win, COLOR_PAIR(4));
        mvwprintw(main_win, y++, 0, "Mit 'a' kannst du Services hinzufügen.");
    } else {
        mvwprintw(main_win, y++, 0,
                  "Nr.  SCOPE SERVICE                                ACTIVE      ENABLED     PORT   DESCRIPTION");
        mvwprintw(main_win, y++, 0,
                  "---- ----- -------------------------------------- ---------- ---------- ------ --------------------------------");

        char summary[MAX_LINE];
        char parts[5][MAX_DESC];

        for (int i = 0; i < num_my_services && y < LINES - 5; i++) {
            const char *svc = my_services[i];
            get_service_summary(svc, summary, sizeof(summary));

            char temp[MAX_LINE];
            strncpy(temp, summary, sizeof(temp) - 1);
            temp[sizeof(temp) - 1] = '\0';

            char *tok = strtok(temp, "|");
            int j = 0;
            while (tok && j < 5) {
                strncpy(parts[j], tok, MAX_DESC - 1);
                parts[j][MAX_DESC - 1] = '\0';
                tok = strtok(NULL, "|");
                j++;
            }
            if (j < 5) continue;

            char scope_disp[8];
            if (strcmp(parts[0], "system") == 0)
                strcpy(scope_disp, "SYS");
            else if (strcmp(parts[0], "user") == 0)
                strcpy(scope_disp, "USR");
            else
                strcpy(scope_disp, "???");

            // Einfach Rohwerte (ohne ANSI) anzeigen
            mvwprintw(main_win, y++, 0,
                      "%-4d %-5s %-38.38s %-10.10s %-10.10s %-6.6s %-.*s",
                      i + 1,
                      scope_disp,
                      svc,
                      parts[1], // active
                      parts[2], // enabled
                      parts[4], // port
                      (int)(COLS - 80 > 0 ? COLS - 80 : 0),
                      parts[3]); // desc
        }
    }

    werase(status_win);
    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 0, 0,
              "Aktionen: [Nummer] Details | a Add | r Remove | R Reload | B Browse All | q Quit");
    wattroff(status_win, COLOR_PAIR(5));

    wrefresh(main_win);
    wrefresh(status_win);
}

// ---------------------------------------------------------
// Eingabe in Statuszeile
// ---------------------------------------------------------
void get_input(char *buf, size_t buflen) {
    if (!buf || buflen == 0) return;

    // Nur Zeile 1 der Statuszeile benutzen, Zeile 0 mit Aktionen bleibt stehen
    wmove(status_win, 1, 0);
    wclrtoeol(status_win);

    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 1, 0, "Eingabe: ");
    wattroff(status_win, COLOR_PAIR(5));
    wrefresh(status_win);

    echo();
    wgetnstr(status_win, buf, (int)buflen - 1);
    noecho();
    buf[buflen - 1] = '\0';

    // Eingabezeile wieder leeren, Aktionszeile bleibt oben stehen
    wmove(status_win, 1, 0);
    wclrtoeol(status_win);
    wrefresh(status_win);
}
// ---------------------------------------------------------
// Favorit hinzufügen (ncurses)
// ---------------------------------------------------------
void add_service_ui(const char *home) {
    (void)home; // wird trotzdem genutzt unten bei save_services

    char svc[MAX_LINE] = {0};

    werase(status_win);
    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 0, 0, "Neuen Service-Namen eingeben (z.B. trainee_trainer-gunicorn.service)");
    mvwprintw(status_win, 1, 0, "> ");
    wattroff(status_win, COLOR_PAIR(5));
    wrefresh(status_win);

    echo();
    wgetnstr(status_win, svc, MAX_LINE - 1);
    noecho();
    svc[MAX_LINE - 1] = '\0';

    // Trim Spaces
    char *start = svc;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    werase(status_win);
    wrefresh(status_win);

    if (strlen(start) == 0) {
        show_message_ui("Abgebrochen – kein Name eingegeben.");
        return;
    }

    // Schon vorhanden?
    for (int i = 0; i < num_my_services; i++) {
        if (strcmp(my_services[i], start) == 0) {
            show_message_ui("Service ist bereits in der Favoritenliste.");
            return;
        }
    }

    char *scope = detect_scope(start);
    if (strcmp(scope, "none") == 0) {
        werase(status_win);
        wattron(status_win, COLOR_PAIR(3));
        mvwprintw(status_win, 0, 0,
                  "Hinweis: Service wird weder als System- noch als User-Service gefunden.");
        mvwprintw(status_win, 1, 0,
                  "Trotzdem hinzufügen? [y/N] ");
        wattroff(status_win, COLOR_PAIR(3));
        wrefresh(status_win);

        int ch = wgetch(status_win);
        werase(status_win);
        wrefresh(status_win);

        if (ch != 'y' && ch != 'Y') {
            show_message_ui("Nicht hinzugefügt.");
            return;
        }
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Erkannt als %s-Service.", scope);
        show_message_ui(msg);
    }

    if (num_my_services < MAX_SERVICES) {
        strncpy(my_services[num_my_services++], start, MAX_LINE - 1);
        my_services[num_my_services - 1][MAX_LINE - 1] = '\0';
        save_services(home);
        show_message_ui("Service zu Favoriten hinzugefügt.");
    } else {
        show_message_ui("Maximale Anzahl Services erreicht.");
    }
}

// ---------------------------------------------------------
// Favorit entfernen (ncurses)
// ---------------------------------------------------------
void remove_service_ui(const char *home) {
    (void)home;

    if (num_my_services == 0) {
        show_message_ui("Keine Favoriten in der Liste.");
        return;
    }

    // Liste in main_win zeigen
    werase(main_win);
    int y = 0;
    wattron(main_win, COLOR_PAIR(1));
    mvwprintw(main_win, y++, 0, "Favoriten entfernen");
    wattroff(main_win, COLOR_PAIR(1));
    y++;

    for (int i = 0; i < num_my_services && y < LINES - 4; i++) {
        mvwprintw(main_win, y++, 0, "  %d) %s", i + 1, my_services[i]);
    }
    wrefresh(main_win);

    // Nummer abfragen
    char buf[MAX_LINE] = {0};
    werase(status_win);
    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 0, 0, "Nummer zum Entfernen (leer = Abbruch): ");
    wattroff(status_win, COLOR_PAIR(5));
    wrefresh(status_win);

    echo();
    wgetnstr(status_win, buf, MAX_LINE - 1);
    noecho();
    buf[MAX_LINE - 1] = '\0';

    werase(status_win);
    wrefresh(status_win);

    if (strlen(buf) == 0) {
        show_message_ui("Abgebrochen.");
        return;
    }

    int idx = atoi(buf);
    if (idx < 1 || idx > num_my_services) {
        show_message_ui("Ungültige Auswahl.");
        return;
    }

    char removed[MAX_LINE];
    strncpy(removed, my_services[idx - 1], MAX_LINE - 1);
    removed[MAX_LINE - 1] = '\0';

    // verschieben
    for (int i = idx - 1; i < num_my_services - 1; i++) {
        strcpy(my_services[i], my_services[i + 1]);
    }
    num_my_services--;

    save_services(home);

    char msg[160];
    snprintf(msg, sizeof(msg), "Service entfernt: %s", removed);
    show_message_ui(msg);
}

// ---------------------------------------------------------
// Alle Services browsen (mit Filter + Pfeiltasten)
// ---------------------------------------------------------
void browse_all_services_ui(const char *home) {
    build_all_services_list(home);

    char filter[256] = "";
    int  selected = 0;
    int  filtered_list[MAX_SERVICES];
    int  filtered_count = 0;

    for (;;) {
        werase(main_win);
        int y = 0;

        wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, y++, 0, "=====================================================");
        mvwprintw(main_win, y++, 0, "        Alle Services – System + User                 ");
        mvwprintw(main_win, y++, 0, "=====================================================");
        wattroff(main_win, COLOR_PAIR(1));
        y++;

        mvwprintw(main_win, y++, 0, "Suche: [%s]  (/ zum Editieren, Pfeile, Enter, b Zurück)", filter);

        filtered_count = 0;
        for (int i = 0; i < num_all_services; i++) {
            if (filter[0] != '\0' && strstr(all_services[i], filter) == NULL)
                continue;
            if (filtered_count < MAX_SERVICES) {
                filtered_list[filtered_count++] = i;
            }
        }

        for (int k = 0; k < filtered_count && y < LINES - 4; k++) {
            int idx = filtered_list[k];

            char summary[MAX_LINE];
            get_service_summary(all_services[idx], summary, sizeof(summary));

            char parts[5][MAX_DESC];
            char temp[MAX_LINE];
            strncpy(temp, summary, sizeof(temp) - 1);
            temp[sizeof(temp) - 1] = '\0';

            char *tok = strtok(temp, "|");
            int j = 0;
            while (tok && j < 5) {
                strncpy(parts[j], tok, MAX_DESC - 1);
                parts[j][MAX_DESC - 1] = '\0';
                tok = strtok(NULL, "|");
                j++;
            }
            if (j < 5) continue;

            char scope_disp[8];
            if (strcmp(parts[0], "system") == 0)
                strcpy(scope_disp, "SYS");
            else if (strcmp(parts[0], "user") == 0)
                strcpy(scope_disp, "USR");
            else
                strcpy(scope_disp, "???");

            if (k == selected) wattron(main_win, A_REVERSE);

            mvwprintw(main_win, y++, 0,
                      "%-4d %-5s %-38.38s %-10.10s %-10.10s %-6.6s %-.*s",
                      k + 1,
                      scope_disp,
                      all_services[idx],
                      parts[1],
                      parts[2],
                      parts[4],
                      (int)(COLS - 80 > 0 ? COLS - 80 : 0),
                      parts[3]);

            if (k == selected) wattroff(main_win, A_REVERSE);
        }

        wrefresh(main_win);

        int ch = wgetch(main_win);
        if (ch == KEY_UP && selected > 0) {
            selected--;
        } else if (ch == KEY_DOWN && selected < filtered_count - 1) {
            selected++;
        } else if (ch == '/') {
            // Filter editieren
            werase(status_win);
            wattron(status_win, COLOR_PAIR(5));
            mvwprintw(status_win, 0, 0, "Filter: ");
            wattroff(status_win, COLOR_PAIR(5));
            wrefresh(status_win);

            echo();
            wgetnstr(status_win, filter, sizeof(filter) - 1);
            noecho();
            filter[sizeof(filter) - 1] = '\0';

            werase(status_win);
            wrefresh(status_win);
            selected = 0;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (filtered_count > 0) {
                service_detail_page_ui(all_services[filtered_list[selected]]);
            }
        } else if (ch == 'b' || ch == 'B' || ch == 27) {
            break;
        }
    }
}

// ---------------------------------------------------------
// Detailseite eines Services (ncurses)
// ---------------------------------------------------------
void service_detail_page_ui(const char *svc) {
    for (;;) {
        werase(main_win);
        int y = 0;

        // Header
        wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, y++, 0, "=====================================================");
        mvwprintw(main_win, y++, 0, "Service-Details: %s", svc);
        mvwprintw(main_win, y++, 0, "=====================================================");
        wattroff(main_win, COLOR_PAIR(1));
        y++;

        char summary[MAX_LINE];
        get_service_summary(svc, summary, sizeof(summary));

        char parts[5][MAX_DESC];
        char temp[MAX_LINE];
        strncpy(temp, summary, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';

        char *token = strtok(temp, "|");
        int j = 0;
        while (token && j < 5) {
            strncpy(parts[j], token, sizeof(parts[0]) - 1);
            parts[j][sizeof(parts[0]) - 1] = '\0';
            token = strtok(NULL, "|");
            j++;
        }
        if (j < 5) {
            show_message_ui("Fehler beim Laden der Summary.");
            return;
        }

        char active_c[64], enabled_c[64];
        status_color(parts[1], active_c, sizeof(active_c));
        enabled_color(parts[2], enabled_c, sizeof(enabled_c));

        char scope_label[64];
        if (strcmp(parts[0], "system") == 0)
            strcpy(scope_label, "System-Service");
        else if (strcmp(parts[0], "user") == 0)
            strcpy(scope_label, "User-Service (~/.config/systemd/user)");
        else
            strcpy(scope_label, "NICHT GEFUNDEN");

        if (strcmp(parts[0], "none") == 0) {
            wattron(main_win, COLOR_PAIR(4));
            mvwprintw(main_win, y++, 0, "ACHTUNG: Service nicht gefunden.");
            wattroff(main_win, COLOR_PAIR(4));
            y++;
        }

        // Übersicht
        wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, y++, 0, "Übersicht");
        wattroff(main_win, COLOR_PAIR(1));

        mvwprintw(main_win, y++, 0, "  Name:        %s",  svc);
        mvwprintw(main_win, y++, 0, "  Scope:       %s",  scope_label);
        mvwprintw(main_win, y++, 0, "  Active:      %s",  active_c);
        mvwprintw(main_win, y++, 0, "  Enabled:     %s",  enabled_c);
        mvwprintw(main_win, y++, 0, "  Port:        %s",  parts[4]);
        mvwprintw(main_win, y++, 0, "  Description: %s",  parts[3]);

        // Zusatzinfos
        char act_ts[MAX_LINE]       = {0};
        char inact_ts[MAX_LINE]     = {0};
        char fragment_path[MAX_LINE]= {0};
        char unit_state[32]         = {0};
        char substate[32]           = {0};
        char cmd[MAX_LINE];

        const char *user_flag = (strcmp(parts[0], "system") == 0 ? "" : "--user");

        if (strcmp(parts[0], "none") != 0) {
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

        wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, y++, 0, "Zusätzliche Infos");
        wattroff(main_win, COLOR_PAIR(1));

        mvwprintw(main_win, y++, 0, "  LoadState:     %s", strlen(unit_state) ? unit_state : "unknown");
        mvwprintw(main_win, y++, 0, "  SubState:      %s", strlen(substate) ? substate : "unknown");
        mvwprintw(main_win, y++, 0, "  Fragment:      %s", strlen(fragment_path) ? fragment_path : "unknown");
        mvwprintw(main_win, y++, 0, "  Active seit:   %s", strlen(act_ts) ? act_ts : "n/a");
        mvwprintw(main_win, y++, 0, "  Inactive seit: %s", strlen(inact_ts) ? inact_ts : "n/a");

        // Logs
        wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, y++, 0, "Letzte Logs (journalctl -u %s -n %d)", svc, DETAIL_LOG_LINES);
        wattroff(main_win, COLOR_PAIR(1));
        y++;

        char log_cmd[MAX_LINE];
        snprintf(log_cmd, sizeof(log_cmd),
                 "journalctl %s -u \"%s\" -n %d --no-pager 2>/dev/null",
                 user_flag, svc, DETAIL_LOG_LINES);

        FILE *log_fp = popen(log_cmd, "r");
        if (log_fp) {
            char log_line[COLS + 1];
            while (fgets(log_line, sizeof(log_line), log_fp) && y < LINES - 5) {
                log_line[strcspn(log_line, "\n")] = '\0';
                mvwprintw(main_win, y++, 0, "%s", log_line);
            }
            pclose(log_fp);
        } else {
            mvwprintw(main_win, y++, 0, "(Keine Logs verfügbar)");
        }

        // Footer
        werase(status_win);
        wattron(status_win, COLOR_PAIR(5));
        mvwprintw(status_win, 0, 0,
                  "s Start | t Stop | r Restart | e Enable | d Disable | S Status | I Show | L Logs | o Browser | c CPU/RAM | D Deps | b Back");
        wattroff(status_win, COLOR_PAIR(5));

        wrefresh(main_win);
        wrefresh(status_win);

        int ch = wgetch(main_win);

        if (ch == 's') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s start \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Gestartet");
        } else if (ch == 't' || ch == 'T') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s stop \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Gestoppt");
        } else if (ch == 'r' || ch == 'R') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s restart \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Neugestartet");
        } else if (ch == 'e' || ch == 'E') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s enable \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Enabled");
        } else if (ch == 'd') {
            snprintf(cmd, sizeof(cmd),
                     "%ssystemctl %s disable \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Disabled");
        } else if (ch == 'S') {
            // systemctl status via less
            def_prog_mode();
            endwin();
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s status \"%s\" | less",
                     user_flag, svc);
            system(cmd);
            reset_prog_mode();
            refresh();
        } else if (ch == 'I') {
            // systemctl show via less
            def_prog_mode();
            endwin();
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show \"%s\" | less",
                     user_flag, svc);
            system(cmd);
            reset_prog_mode();
            refresh();
        } else if (ch == 'L') {
            // live Logs
            def_prog_mode();
            endwin();
            snprintf(cmd, sizeof(cmd),
                     "journalctl %s -u \"%s\" -f",
                     user_flag, svc);
            system(cmd);
            reset_prog_mode();
            refresh();
        } else if (ch == 'o' || ch == 'O') {
            open_in_browser(parts[4]);
        } else if (ch == 'c' || ch == 'C') {
            char pid_str[32] = {0};
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p MainPID --value \"%s\"",
                     user_flag, svc);
            execute_cmd(cmd, pid_str, sizeof(pid_str));
            long pid = atol(pid_str);

            float cpu = 0.0f;
            long  rss = 0;
            char msg[128];

            if (pid > 0 && get_resource_usage(pid, &cpu, &rss) == 0) {
                snprintf(msg, sizeof(msg), "PID %ld – CPU: %.1f%% | RAM: %ld KB", pid, cpu, rss);
            } else {
                snprintf(msg, sizeof(msg), "PID nicht verfügbar oder keine Ressourcen-Daten.");
            }
            show_message_ui(msg);
        } else if (ch == 'D') {
            show_dependencies(svc, parts[0]);
        } else if (ch == 'b' || ch == 'B' || ch == 27) {
            break;
        } else {
            show_message_ui("Unbekannte Aktion");
        }
    }
}
