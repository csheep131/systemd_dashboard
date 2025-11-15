#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ui.h"
#include "utils.h"
int execute_cmd(const char *cmd, char *output, size_t max_output);
// Diese Werte müssen mit sys_dashboard.c übereinstimmen
#define MAX_SERVICES 1000
#define MAX_LINE     1024
#define MAX_DESC     256
#define DETAIL_LOG_LINES 20

// Globale Arrays & Variablen aus sys_dashboard.c
extern char my_services[MAX_SERVICES][MAX_LINE];
extern int  num_my_services;
extern char all_services[MAX_SERVICES][MAX_LINE];
extern int  num_all_services;
extern const char *sudo_flag;

// Funktionen aus sys_dashboard.c
extern void get_service_summary(const char *svc, char *summary, size_t bufsize);
extern char *detect_scope(const char *svc);
extern char *guess_port(const char *svc, const char *scope);
extern void build_all_services_list(const char *home);
extern void load_services(const char *home);
extern void save_services(const char *home);

// ncurses-Fenster
static WINDOW *main_win = NULL;
static WINDOW *status_win = NULL;

// Farb-Paare
// 1: Header (cyan)
// 2: OK (grün)
// 3: Warnung (gelb)
// 4: Fehler/Hinweis (rot)
// 5: Standard (weiß)
// 6: Auswahl-Hintergrund (invertiert / blue-ish, je nach Terminal)
static void init_colors(void) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN,   -1);
    init_pair(2, COLOR_GREEN,  -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_RED,    -1);
    init_pair(5, COLOR_WHITE,  -1);
    init_pair(6, COLOR_BLACK,  COLOR_CYAN); // Auswahlzeile
}

// --------------------------------------------------
// Basis-UI
// --------------------------------------------------
void init_ui(void) {
    initscr();
    if (!has_colors()) {
        endwin();
        printf("Terminal unterstützt keine Farben.\n");
        exit(1);
    }
    init_colors();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int rows = LINES;
    int cols = COLS;

    main_win   = newwin(rows - 3, cols, 0, 0);
    status_win = newwin(3, cols, rows - 3, 0);

    scrollok(main_win, TRUE);
    keypad(main_win, TRUE);
    keypad(status_win, TRUE);

    wrefresh(main_win);
    wrefresh(status_win);
}

void end_ui(void) {
    if (main_win)   { delwin(main_win);   main_win = NULL; }
    if (status_win) { delwin(status_win); status_win = NULL; }
    endwin();
}

// Einfache Status-/Hinweisbox unten
void show_message_ui(const char *msg) {
    if (!status_win) return;
    werase(status_win);
    wattron(status_win, COLOR_PAIR(4) | A_BOLD);
    mvwprintw(status_win, 1, 1, "%s – [Enter] zum Fortfahren", msg);
    wattroff(status_win, COLOR_PAIR(4) | A_BOLD);
    wrefresh(status_win);
    wgetch(status_win);
    werase(status_win);
    wrefresh(status_win);
}

// --------------------------------------------------
// Dashboard-Rendering
// --------------------------------------------------

// Kleine Hilfsfunktion: Status-Text → Farb-Attribut
static int color_for_active(const char *state) {
    if (strcmp(state, "active") == 0)
        return 2;  // grün
    if (strcmp(state, "inactive") == 0)
        return 5;  // weiß/neutral
    if (strcmp(state, "failed") == 0)
        return 4;  // rot
    if (strcmp(state, "activating") == 0 || strcmp(state, "deactivating") == 0)
        return 3;  // gelb
    return 3;      // unknown → gelb
}

static int color_for_enabled(const char *state) {
    if (strcmp(state, "enabled") == 0)
        return 2;
    if (strcmp(state, "disabled") == 0)
        return 5;
    if (strcmp(state, "static") == 0 || strcmp(state, "indirect") == 0 || strcmp(state, "generated") == 0)
        return 3;
    return 3;
}

// Port-Farbe: "-" → grau/weiß, sonst grün
static int color_for_port(const char *port) {
    if (!port || strcmp(port, "-") == 0 || strlen(port) == 0)
        return 5;  // neutral
    return 2;      // grün: Port erkannt
}

// --------------------------------------------------
// Dashboard: Favoriten-Liste
// --------------------------------------------------
void render_dashboard_ui(int selected_idx, int focus_on_list) {
    if (!main_win || !status_win) return;

    werase(main_win);
    int maxy, maxx;
    getmaxyx(main_win, maxy, maxx);

    int y = 0;

    // Header
    wattron(main_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(main_win, y++, 0, "=====================================================");
    mvwprintw(main_win, y++, 0, "        Systemd Dashboard – Eigene Services          ");
    mvwprintw(main_win, y++, 0, "=====================================================");
    wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
    y++;

    // Kurze Erklärung
    wattron(main_win, COLOR_PAIR(5));
    mvwprintw(main_win, y++, 0, "Fokus: %s  (Tab zum Wechseln: Liste <-> Aktionen)",
              focus_on_list ? "Liste" : "Aktionen");
    wattroff(main_win, COLOR_PAIR(5));
    y++;

    if (num_my_services == 0) {
        wattron(main_win, COLOR_PAIR(4));
        mvwprintw(main_win, y++, 0, "Keine Services konfiguriert.");
        wattroff(main_win, COLOR_PAIR(4));
        mvwprintw(main_win, y++, 0, "Mit 'a' kannst du Services hinzufügen.");
    } else {
        // Tabellenkopf
        wattron(main_win, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(main_win, y,   0, "Nr.");
        mvwprintw(main_win, y,   5, "SCOPE");
        mvwprintw(main_win, y,  12, "SERVICE");
        mvwprintw(main_win, y,  48, "ACTIVE");
        mvwprintw(main_win, y,  60, "ENABLED");
        mvwprintw(main_win, y,  74, "PORT");
        mvwprintw(main_win, y,  82, "DESCRIPTION");
        y++;
        mvwhline(main_win, y++, 0, '-', maxx);
        wattroff(main_win, COLOR_PAIR(5) | A_BOLD);

        // Einträge
        for (int i = 0; i < num_my_services && y < maxy - 1; i++) {
            const char *svc = my_services[i];
            char summary[MAX_LINE];
            char parts[5][MAX_DESC];

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

            const char *scope_str = parts[0];
            const char *active    = parts[1];
            const char *enabled   = parts[2];
            const char *desc      = parts[3];
            const char *port      = parts[4];

            char scope_disp[8];
            if (strcmp(scope_str, "system") == 0)      strcpy(scope_disp, "SYS");
            else if (strcmp(scope_str, "user") == 0)   strcpy(scope_disp, "USR");
            else                                       strcpy(scope_disp, "???");

            int is_selected = (i == selected_idx && focus_on_list);

            if (is_selected) {
                wattron(main_win, COLOR_PAIR(6) | A_BOLD);
            }

            // Nummer + Scope + Service
            mvwprintw(main_win, y, 0, "%-3d", i + 1);
            mvwprintw(main_win, y, 5, "%-4s", scope_disp);
            mvwprintw(main_win, y, 12, "%-30.30s", svc);

            // Active
            int c_active = color_for_active(active);
            wattron(main_win, COLOR_PAIR(c_active));
            mvwprintw(main_win, y, 48, "%-10.10s", active);
            wattroff(main_win, COLOR_PAIR(c_active));

            // Enabled
            int c_enabled = color_for_enabled(enabled);
            wattron(main_win, COLOR_PAIR(c_enabled));
            mvwprintw(main_win, y, 60, "%-10.10s", enabled);
            wattroff(main_win, COLOR_PAIR(c_enabled));

            // Port
            int c_port = color_for_port(port);
            wattron(main_win, COLOR_PAIR(c_port));
            mvwprintw(main_win, y, 74, "%-6.6s", port);
            wattroff(main_win, COLOR_PAIR(c_port));

            // Description (ggf. kürzen)
            mvwprintw(main_win, y, 82, "%.*s", maxx - 83, desc);

            if (is_selected) {
                wattroff(main_win, COLOR_PAIR(6) | A_BOLD);
            }

            y++;
        }
    }

    wrefresh(main_win);

    // Status-Zeile unten
    werase(status_win);
    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 0, 0,
              "Pfeile/jk: Auswahl | Enter: Details | o: Browser | a: Add | r: Remove | R: Reload | B: Browse All | Tab: Fokus | q: Quit");
    wattroff(status_win, COLOR_PAIR(5));
    wrefresh(status_win);
}

// --------------------------------------------------
// Browse-All-Services (mit Filter und o/a)
// --------------------------------------------------
void browse_all_services_ui(const char *home) {
    build_all_services_list(home);

    char filter[256] = "";
    int filtered_idx[MAX_SERVICES];
    int filtered_count = 0;
    int selected = 0;

    while (1) {
        // Filter anwenden
        filtered_count = 0;
        for (int i = 0; i < num_all_services && filtered_count < MAX_SERVICES; i++) {
            if (filter[0] != '\0' && strstr(all_services[i], filter) == NULL) {
                continue;
            }
            filtered_idx[filtered_count++] = i;
        }
        if (selected >= filtered_count && filtered_count > 0) {
            selected = filtered_count - 1;
        }
        if (selected < 0) selected = 0;

        werase(main_win);
        int maxy, maxx;
        getmaxyx(main_win, maxy, maxx);
        int y = 0;

        wattron(main_win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(main_win, y++, 0, "=====================================================");
        mvwprintw(main_win, y++, 0, "        Alle Services – System + User                ");
        mvwprintw(main_win, y++, 0, "=====================================================");
        wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
        y++;

        wattron(main_win, COLOR_PAIR(5));
        mvwprintw(main_win, y++, 0,
                  "Suche: [%s]  (/ zum Editieren, Pfeile/jk, Enter=Details, a=Fav, o=Browser, b=Zurück)", filter);
        wattroff(main_win, COLOR_PAIR(5));
        y++;

        if (filtered_count == 0) {
            wattron(main_win, COLOR_PAIR(4));
            mvwprintw(main_win, y++, 0, "Keine Services gefunden.");
            wattroff(main_win, COLOR_PAIR(4));
        } else {
            // Kopfzeile
            wattron(main_win, COLOR_PAIR(5) | A_BOLD);
            mvwprintw(main_win, y,   0, "Nr.");
            mvwprintw(main_win, y,   5, "SCOPE");
            mvwprintw(main_win, y,  12, "SERVICE");
            mvwprintw(main_win, y,  48, "ACTIVE");
            mvwprintw(main_win, y,  60, "ENABLED");
            mvwprintw(main_win, y,  74, "PORT");
            mvwprintw(main_win, y,  82, "DESCRIPTION");
            y++;
            mvwhline(main_win, y++, 0, '-', maxx);
            wattroff(main_win, COLOR_PAIR(5) | A_BOLD);

            // Liste
            for (int k = 0; k < filtered_count && y < maxy - 1; k++) {
                int idx = filtered_idx[k];
                const char *svc = all_services[idx];

                char summary[MAX_LINE];
                char parts[5][MAX_DESC];
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

                const char *scope_str = parts[0];
                const char *active    = parts[1];
                const char *enabled   = parts[2];
                const char *desc      = parts[3];
                const char *port      = parts[4];

                char scope_disp[8];
                if (strcmp(scope_str, "system") == 0)      strcpy(scope_disp, "SYS");
                else if (strcmp(scope_str, "user") == 0)   strcpy(scope_disp, "USR");
                else                                       strcpy(scope_disp, "???");

                int is_selected = (k == selected);
                if (is_selected) {
                    wattron(main_win, COLOR_PAIR(6) | A_BOLD);
                }

                mvwprintw(main_win, y, 0, "%-3d", k + 1);
                mvwprintw(main_win, y, 5, "%-4s", scope_disp);
                mvwprintw(main_win, y, 12, "%-30.30s", svc);

                int c_active  = color_for_active(active);
                int c_enabled = color_for_enabled(enabled);
                int c_port    = color_for_port(port);

                wattron(main_win, COLOR_PAIR(c_active));
                mvwprintw(main_win, y, 48, "%-10.10s", active);
                wattroff(main_win, COLOR_PAIR(c_active));

                wattron(main_win, COLOR_PAIR(c_enabled));
                mvwprintw(main_win, y, 60, "%-10.10s", enabled);
                wattroff(main_win, COLOR_PAIR(c_enabled));

                wattron(main_win, COLOR_PAIR(c_port));
                mvwprintw(main_win, y, 74, "%-6.6s", port);
                wattroff(main_win, COLOR_PAIR(c_port));

                mvwprintw(main_win, y, 82, "%.*s", maxx - 83, desc);

                if (is_selected) {
                    wattroff(main_win, COLOR_PAIR(6) | A_BOLD);
                }

                y++;
            }
        }

        wrefresh(main_win);

        werase(status_win);
        wattron(status_win, COLOR_PAIR(5));
        mvwprintw(status_win, 0, 0,
                  "Browse All: Pfeile/jk=Navigate | Enter=Details | / Filter | a=Favorit | o=Browser | b/Esc=Zurück");
        wattroff(status_win, COLOR_PAIR(5));
        wrefresh(status_win);

        int ch = wgetch(main_win);

        if (ch == 'b' || ch == 'B' || ch == 27) {
            break;
        } else if ((ch == KEY_UP || ch == 'k') && filtered_count > 0) {
            if (selected > 0) selected--;
            else selected = filtered_count - 1;
        } else if ((ch == KEY_DOWN || ch == 'j') && filtered_count > 0) {
            if (selected < filtered_count - 1) selected++;
            else selected = 0;
        } else if (ch == '/' ) {
            // Filter bearbeiten
            werase(status_win);
            wattron(status_win, COLOR_PAIR(5));
            mvwprintw(status_win, 1, 0, "Filter: ");
            wattroff(status_win, COLOR_PAIR(5));
            wrefresh(status_win);

            echo();
            curs_set(1);
            wgetnstr(status_win, filter, sizeof(filter) - 1);
            noecho();
            curs_set(0);
            selected = 0;
        } else if ((ch == '\n' || ch == KEY_ENTER) && filtered_count > 0) {
            const char *svc = all_services[filtered_idx[selected]];
            service_detail_page_ui(svc);
        } else if ((ch == 'o' || ch == 'O') && filtered_count > 0) {
            const char *svc = all_services[filtered_idx[selected]];
            char *scope = detect_scope(svc);
            char *port  = guess_port(svc, scope);
            if (port && strcmp(port, "-") != 0 && strlen(port) > 0) {
                open_in_browser(port);
            } else {
                show_message_ui("Kein Port erkannt oder Service lauscht nicht.");
            }
        } else if ((ch == 'a' || ch == 'A') && filtered_count > 0) {
            const char *svc = all_services[filtered_idx[selected]];

            // Prüfen, ob schon in Favoriten
            int exists = 0;
            for (int i = 0; i < num_my_services; i++) {
                if (strcmp(my_services[i], svc) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (exists) {
                show_message_ui("Service ist bereits in den Favoriten.");
            } else if (num_my_services >= MAX_SERVICES) {
                show_message_ui("Maximale Anzahl Favoriten erreicht.");
            } else {
                strncpy(my_services[num_my_services], svc, MAX_LINE - 1);
                my_services[num_my_services][MAX_LINE - 1] = '\0';
                num_my_services++;
                save_services(home);
                show_message_ui("Service zu Favoriten hinzugefügt.");
            }
        }
    }
}

// --------------------------------------------------
// Detailseite (ncurses)
// --------------------------------------------------
void service_detail_page_ui(const char *svc) {
    if (!main_win || !status_win) return;

    while (1) {
        werase(main_win);
        int maxy, maxx;
        getmaxyx(main_win, maxy, maxx);
        int y = 0;

        wattron(main_win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(main_win, y++, 0, "=====================================================");
        mvwprintw(main_win, y++, 0, "Service-Details: %s", svc);
        mvwprintw(main_win, y++, 0, "=====================================================");
        wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
        y++;

        char summary[MAX_LINE];
        char parts[5][MAX_DESC];
        get_service_summary(svc, summary, sizeof(summary));
        char temp[MAX_LINE];
        strncpy(temp, summary, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';

        char *token = strtok(temp, "|");
        int j = 0;
        while (token && j < 5) {
            strncpy(parts[j], token, MAX_DESC - 1);
            parts[j][MAX_DESC - 1] = '\0';
            token = strtok(NULL, "|");
            j++;
        }
        if (j < 5) {
            show_message_ui("Fehler beim Laden der Summary.");
            return;
        }

        const char *scope_str = parts[0];
        const char *active    = parts[1];
        const char *enabled   = parts[2];
        const char *desc      = parts[3];
        const char *port      = parts[4];

        char scope_label[64];
        if (strcmp(scope_str, "system") == 0)
            strcpy(scope_label, "System-Service");
        else if (strcmp(scope_str, "user") == 0)
            strcpy(scope_label, "User-Service (~/.config/systemd/user)");
        else
            strcpy(scope_label, "NICHT GEFUNDEN");

        if (strcmp(scope_str, "none") == 0) {
            wattron(main_win, COLOR_PAIR(4) | A_BOLD);
            mvwprintw(main_win, y++, 0, "ACHTUNG: Service wird weder als System- noch als User-Service gefunden.");
            wattroff(main_win, COLOR_PAIR(4) | A_BOLD);
            y++;
        }

        // Übersicht
        wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, y++, 0, "Übersicht");
        wattroff(main_win, COLOR_PAIR(1));

        mvwprintw(main_win, y++, 0, "  Name:        %s", svc);
        mvwprintw(main_win, y++, 0, "  Scope:       %s", scope_label);

        int c_active = color_for_active(active);
        wattron(main_win, COLOR_PAIR(c_active));
        mvwprintw(main_win, y++, 0, "  Active:      %s", active);
        wattroff(main_win, COLOR_PAIR(c_active));

        int c_enabled = color_for_enabled(enabled);
        wattron(main_win, COLOR_PAIR(c_enabled));
        mvwprintw(main_win, y++, 0, "  Enabled:     %s", enabled);
        wattroff(main_win, COLOR_PAIR(c_enabled));

        int c_port = color_for_port(port);
        wattron(main_win, COLOR_PAIR(c_port));
        mvwprintw(main_win, y++, 0, "  Port:        %s", port);
        wattroff(main_win, COLOR_PAIR(c_port));

        mvwprintw(main_win, y++, 0, "  Description: %s", desc);

        // Zusätzliche Infos
        char act_ts[MAX_LINE]      = {0};
        char inact_ts[MAX_LINE]    = {0};
        char fragment_path[MAX_LINE] = {0};
        char unit_state[32]        = {0};
        char substate[32]          = {0};
        char cmd[MAX_LINE];

        const char *user_flag = (strcmp(scope_str, "system") == 0 ? "" : "--user");

        if (strcmp(scope_str, "none") != 0) {
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
            char log_line[MAX_LINE];
            while (fgets(log_line, sizeof(log_line), log_fp) && y < maxy - 2) {
                log_line[strcspn(log_line, "\n")] = '\0';
                mvwprintw(main_win, y++, 0, "%s", log_line);
            }
            pclose(log_fp);
        } else {
            mvwprintw(main_win, y++, 0, "(Keine Logs verfügbar)");
        }

        wrefresh(main_win);

        werase(status_win);
        wattron(status_win, COLOR_PAIR(5));
        mvwprintw(status_win, 0, 0,
                  "s Start | t Stop | r Restart | e Enable | d Disable | S Status | I Show | L Live-Logs | o Browser | c CPU/RAM | D Deps | b Zurück");
        wattroff(status_win, COLOR_PAIR(5));
        wrefresh(status_win);

        int ch = wgetch(status_win);

        if (ch == 'b' || ch == 'B' || ch == 27) {
            break;
        } else if (ch == 's') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s start \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Gestartet");
        } else if (ch == 't' || ch == 'T') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s stop \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Gestoppt");
        } else if (ch == 'r' || ch == 'R') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s restart \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Neugestartet");
        } else if (ch == 'e' || ch == 'E') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s enable \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Enabled");
        } else if (ch == 'd') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s disable \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Disabled");
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
            if (port && strcmp(port, "-") != 0 && strlen(port) > 0) {
                open_in_browser(port);
            } else {
                show_message_ui("Kein Port erkannt oder Service lauscht nicht.");
            }
        } else if (ch == 'c' || ch == 'C') {
            char pid_str[32] = {0};
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p MainPID --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, pid_str, sizeof(pid_str));
            long pid = atol(pid_str);
            if (pid <= 0) {
                show_message_ui("PID nicht verfügbar.");
            } else {
                float cpu = 0.0f;
                long rss  = 0;
                if (get_resource_usage(pid, &cpu, &rss) == 0) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "CPU: %.1f%% | RAM: %ld KB", cpu, rss);
                    show_message_ui(msg);
                } else {
                    show_message_ui("Ressourcen nicht ermittelbar.");
                }
            }
        } else if (ch == 'D') {
            show_dependencies(svc, scope_str);
        }
    }
}

// --------------------------------------------------
// Add / Remove Favoriten (ncurses)
// --------------------------------------------------

// Service zu Favoriten hinzufügen (Eingabe per ncurses)
void add_service_ui(const char *home) {
    if (!main_win || !status_win) return;

    char svc[MAX_LINE] = {0};

    werase(status_win);
    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 0, 0, "Service zu Favoriten hinzufügen");
    mvwprintw(status_win, 1, 0, "Name (z.B. trainee_trainer-gunicorn.service): ");
    wattroff(status_win, COLOR_PAIR(5));
    wrefresh(status_win);

    echo();
    curs_set(1);
    wgetnstr(status_win, svc, sizeof(svc) - 1);
    noecho();
    curs_set(0);

    // Whitespace trimmen
    for (int i = strlen(svc) - 1; i >= 0 && isspace((unsigned char)svc[i]); i--) {
        svc[i] = '\0';
    }

    if (strlen(svc) == 0) {
        show_message_ui("Abgebrochen (kein Name eingegeben).");
        return;
    }

    // Already exists?
    for (int i = 0; i < num_my_services; i++) {
        if (strcmp(my_services[i], svc) == 0) {
            show_message_ui("Service ist bereits in der Favoritenliste.");
            return;
        }
    }

    char *scope = detect_scope(svc);
    if (strcmp(scope, "none") == 0) {
        werase(status_win);
        wattron(status_win, COLOR_PAIR(3));
        mvwprintw(status_win, 0, 0,
                  "Service wird weder als System- noch als User-Service gefunden.");
        mvwprintw(status_win, 1, 0,
                  "Trotzdem zu Favoriten hinzufügen? (y/N): ");
        wattroff(status_win, COLOR_PAIR(3));
        wrefresh(status_win);
        int ch = wgetch(status_win);
        if (ch != 'y' && ch != 'Y') {
            show_message_ui("Nicht hinzugefügt.");
            return;
        }
    }

    if (num_my_services >= MAX_SERVICES) {
        show_message_ui("Maximale Anzahl Services erreicht.");
        return;
    }

    strncpy(my_services[num_my_services], svc, MAX_LINE - 1);
    my_services[num_my_services][MAX_LINE - 1] = '\0';
    num_my_services++;
    save_services(home);

    show_message_ui("Service zu Favoriten hinzugefügt.");
}

// Service aus Favoriten entfernen (Auswahl per Zahl)
void remove_service_ui(const char *home) {
    if (!main_win || !status_win) return;

    if (num_my_services == 0) {
        show_message_ui("Keine Favoriten in der Liste.");
        return;
    }

    werase(main_win);
    int maxy, maxx;
    getmaxyx(main_win, maxy, maxx);
    int y = 0;

    wattron(main_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(main_win, y++, 0, "=====================================================");
    mvwprintw(main_win, y++, 0, "Service aus Favoriten entfernen");
    mvwprintw(main_win, y++, 0, "=====================================================");
    wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
    y++;

    for (int i = 0; i < num_my_services && y < maxy - 1; i++) {
        mvwprintw(main_win, y++, 0, "%3d) %s", i + 1, my_services[i]);
    }
    wrefresh(main_win);

    werase(status_win);
    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 0, 0, "Nummer zum Entfernen eingeben (0 = Abbrechen): ");
    wattroff(status_win, COLOR_PAIR(5));
    wrefresh(status_win);

    char buf[16] = {0};
    echo();
    curs_set(1);
    wgetnstr(status_win, buf, sizeof(buf) - 1);
    noecho();
    curs_set(0);

    int idx = atoi(buf);
    if (idx <= 0) {
        show_message_ui("Abgebrochen.");
        return;
    }
    if (idx < 1 || idx > num_my_services) {
        show_message_ui("Ungültige Auswahl.");
        return;
    }

    idx--; // 0-based
    char removed[MAX_LINE];
    strncpy(removed, my_services[idx], sizeof(removed) - 1);
    removed[sizeof(removed) - 1] = '\0';

    for (int i = idx; i < num_my_services - 1; i++) {
        strncpy(my_services[i], my_services[i + 1], MAX_LINE);
    }
    num_my_services--;
    save_services(home);

    char msg[256];
    snprintf(msg, sizeof(msg), "Service entfernt: %s", removed);
    show_message_ui(msg);
}

// Optional: generische Texteingabe (falls irgendwo benötigt)
void get_input(char *buf, size_t bufsize) {
    if (!status_win) return;
    werase(status_win);
    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 1, 0, "Eingabe: ");
    wattroff(status_win, COLOR_PAIR(5));
    wrefresh(status_win);

    echo();
    curs_set(1);
    wgetnstr(status_win, buf, bufsize - 1);
    noecho();
    curs_set(0);

    // Newline/Spaces abschneiden
    for (int i = strlen(buf) - 1; i >= 0 && isspace((unsigned char)buf[i]); i--) {
        buf[i] = '\0';
    }

    werase(status_win);
    wrefresh(status_win);
}
