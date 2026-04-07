#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <signal.h>

#include "ui.h"
#include "utils.h"
#include "sys_dashboard.h"

// Externe Deklarationen aus sys_dashboard.c
extern void get_service_summary(const char *svc, char *summary, size_t bufsize);
extern char *detect_scope(const char *svc);
extern char *guess_port(const char *svc, const char *scope);
extern void build_all_services_list(const char *home);
extern void load_services(const char *home);
extern void save_services(const char *home);
extern void invalidate_cache(void);
extern void invalidate_service_cache(const char *svc);
extern int execute_cmd(const char *cmd, char *output, size_t max_output);

#define DETAIL_LOG_LINES 20

// ncurses-Fenster
static WINDOW *main_win = NULL;
static WINDOW *status_win = NULL;

// Farb-Paare
// 1: Header (cyan)
// 2: OK (gruen)
// 3: Warnung (gelb)
// 4: Fehler/Hinweis (rot)
// 5: Standard (weiss)
// 6: Auswahl-Hintergrund (invertiert)
static void init_colors(void) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN,   -1);
    init_pair(2, COLOR_GREEN,  -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_RED,    -1);
    init_pair(5, COLOR_WHITE,  -1);
    init_pair(6, COLOR_BLACK,  COLOR_CYAN);
}

// --------------------------------------------------
// Basis-UI
// --------------------------------------------------
void init_ui(void) {
    initscr();
    if (!has_colors()) {
        endwin();
        printf("Terminal unterstuetzt keine Farben.\n");
        exit(1);
    }
    init_colors();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int rows = LINES;
    int cols = COLS;

    // Fenster groesserer mit 3 Zeilen fuer Statusleiste
    int status_height = (rows > 5) ? 3 : 1;
    int main_height = rows - status_height;

    main_win   = newwin(main_height, cols, 0, 0);
    status_win = newwin(status_height, cols, main_height, 0);

    scrollok(main_win, TRUE);
    keypad(main_win, TRUE);
    keypad(status_win, TRUE);

    wrefresh(main_win);
    wrefresh(status_win);

    // halfdelay(5) = 0.5s timeout - seltener pollen spart CPU
    halfdelay(5);
    nodelay(stdscr, TRUE);
}

void end_ui(void) {
    if (main_win)   { delwin(main_win);   main_win = NULL; }
    if (status_win) { delwin(status_win); status_win = NULL; }
    endwin();
}

void show_message_ui(const char *msg) {
    if (!status_win) return;
    werase(status_win);
    wattron(status_win, COLOR_PAIR(4) | A_BOLD);
    mvwprintw(status_win, 0, 1, "%s -- [Enter]", msg);
    wattroff(status_win, COLOR_PAIR(4) | A_BOLD);
    wrefresh(status_win);

    // Warte auf Enter
    for (;;) {
        int ch = wgetch(status_win);
        if (ch == '\n' || ch == KEY_ENTER || ch == ' ')
            break;
    }
    werase(status_win);
    wrefresh(status_win);
}

// Hilfsfunktion zum Parsen der Summary
static void parse_summary(const char *summary, char parts[5][MAX_DESC]) {
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
    while (j < 5) {
        parts[j][0] = '\0';
        j++;
    }
}

// --------------------------------------------------
// Dashboard-Rendering
// --------------------------------------------------

static int color_for_active(const char *state) {
    if (strcmp(state, "active") == 0) return 2;
    if (strcmp(state, "inactive") == 0) return 5;
    if (strcmp(state, "failed") == 0) return 4;
    return 3;
}

static int color_for_enabled(const char *state) {
    if (strcmp(state, "enabled") == 0) return 2;
    if (strcmp(state, "disabled") == 0) return 5;
    return 3;
}

static int color_for_port(const char *port) {
    if (!port || strcmp(port, "-") == 0 || strlen(port) == 0) return 5;
    return 2;
}

void render_dashboard_ui(int selected_idx, int focus_on_list) {
    if (!main_win || !status_win) return;

    werase(main_win);
    int maxy, maxx;
    getmaxyx(main_win, maxy, maxx);

    // Bei resize: Fenster anpassen
    if (need_resize) {
        need_resize = 0;
        endwin();
        clearok(stdscr, TRUE);
        refresh();
        resize_term(0, 0);
        maxy = LINES; maxx = COLS;
        int status_height = (maxy > 5) ? 3 : 1;
        wresize(main_win, maxy - status_height, maxx);
        wresize(status_win, status_height, maxx);
        mvwin(status_win, maxy - status_height, 0);
    }

    int y = 0;

    // Header
    wattron(main_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(main_win, y++, 0, "=====================================================");
    mvwprintw(main_win, y++, 0, "        Systemd Dashboard  Eigene Services          ");
    mvwprintw(main_win, y++, 0, "=====================================================");
    wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
    y++;

    if (num_my_services == 0) {
        wattron(main_win, COLOR_PAIR(4));
        mvwprintw(main_win, y++, 0, "Keine Services konfiguriert.");
        wattroff(main_win, COLOR_PAIR(4));
        mvwprintw(main_win, y++, 0, "Mit 'a' kannst du Services hinzufuegen.");
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
            parse_summary(summary, parts);

            const char *scope_str = parts[0];
            const char *active    = parts[1];
            const char *enabled   = parts[2];
            const char *desc      = parts[3];
            const char *port      = parts[4];

            const char *scope_disp;
            if (strcmp(scope_str, "system") == 0)      scope_disp = "SYS";
            else if (strcmp(scope_str, "user") == 0)    scope_disp = "USR";
            else                                         scope_disp = "???";

            int is_selected = (i == selected_idx && focus_on_list);

            if (is_selected) {
                wattron(main_win, COLOR_PAIR(6) | A_BOLD);
            }

            mvwprintw(main_win, y, 0, "%-3d", i + 1);
            mvwprintw(main_win, y, 5, "%-4s", scope_disp);
            mvwprintw(main_win, y, 12, "%-30.30s", svc);

            int c_active = color_for_active(active);
            wattron(main_win, COLOR_PAIR(c_active));
            mvwprintw(main_win, y, 48, "%-10.10s", active);
            wattroff(main_win, COLOR_PAIR(c_active));

            int c_enabled = color_for_enabled(enabled);
            wattron(main_win, COLOR_PAIR(c_enabled));
            mvwprintw(main_win, y, 60, "%-10.10s", enabled);
            wattroff(main_win, COLOR_PAIR(c_enabled));

            int c_port = color_for_port(port);
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

    wborder(main_win, 0,0,0,0,0,0,0,0);
    wrefresh(main_win);

    // Status-Zeile
    werase(status_win);
    wattron(status_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(status_win, 0, 0, " Pfeile/jk: Auswahl | Enter: Details | o: Browser | a: Add | x: Remove | R: Reload | B: Browse | Tab: Fokus | r: Restart | q: Quit");
    wattroff(status_win, COLOR_PAIR(1) | A_BOLD);
    box(status_win, 0, 0);
    wrefresh(status_win);
}

// --------------------------------------------------
// Browse-All-Services
// --------------------------------------------------
void browse_all_services_ui(const char *home) {
    build_all_services_list(home);

    char filter[256] = "";
    int filtered_idx[MAX_SERVICES];
    int filtered_count = 0;
    int selected = 0;

    while (1) {
        filtered_count = 0;
        for (int i = 0; i < num_all_services && filtered_count < MAX_SERVICES; i++) {
            if (filter[0] != '\0' && strstr(all_services[i], filter) == NULL) {
                continue;
            }
            filtered_idx[filtered_count++] = i;
        }
        if (selected >= filtered_count && filtered_count > 0) selected = filtered_count - 1;
        if (selected < 0) selected = 0;

        werase(main_win);
        int maxy, maxx;
        getmaxyx(main_win, maxy, maxx);
        int y = 0;

        wattron(main_win, COLOR_PAIR(1) | A_BOLD);
        mvwprintw(main_win, y++, 0, "=====================================================");
        mvwprintw(main_win, y++, 0, "        Alle Services System + User                ");
        mvwprintw(main_win, y++, 0, "=====================================================");
        wattroff(main_win, COLOR_PAIR(1) | A_BOLD);
        y++;

        wattron(main_win, COLOR_PAIR(5));
        mvwprintw(main_win, y++, 0,
                  "Suche: [%s]  (/ Filter, Pfeile/jk, Enter=Details, a=Fav, o=Browser, q=Zurueck)", filter);
        wattroff(main_win, COLOR_PAIR(5));
        y++;

        if (filtered_count == 0) {
            wattron(main_win, COLOR_PAIR(4));
            mvwprintw(main_win, y++, 0, "Keine Services gefunden.");
            wattroff(main_win, COLOR_PAIR(4));
        } else {
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

            for (int k = 0; k < filtered_count && y < maxy - 1; k++) {
                int idx = filtered_idx[k];
                const char *svc = all_services[idx];

                char summary[MAX_LINE];
                char parts[5][MAX_DESC];
                get_service_summary(svc, summary, sizeof(summary));
                parse_summary(summary, parts);

                const char *scope_str = parts[0];
                const char *active    = parts[1];
                const char *enabled   = parts[2];
                const char *desc      = parts[3];
                const char *port      = parts[4];

                const char *scope_disp;
                if (strcmp(scope_str, "system") == 0)      scope_disp = "SYS";
                else if (strcmp(scope_str, "user") == 0)    scope_disp = "USR";
                else                                         scope_disp = "???";

                int is_selected = (k == selected);
                if (is_selected) wattron(main_win, COLOR_PAIR(6) | A_BOLD);

                mvwprintw(main_win, y, 0, "%-3d", k + 1);
                mvwprintw(main_win, y, 5, "%-4s", scope_disp);
                mvwprintw(main_win, y, 12, "%-30.30s", svc);

                wattron(main_win, COLOR_PAIR(color_for_active(active)));
                mvwprintw(main_win, y, 48, "%-10.10s", active);
                wattroff(main_win, COLOR_PAIR(color_for_active(active)));

                wattron(main_win, COLOR_PAIR(color_for_enabled(enabled)));
                mvwprintw(main_win, y, 60, "%-10.10s", enabled);
                wattroff(main_win, COLOR_PAIR(color_for_enabled(enabled)));

                wattron(main_win, COLOR_PAIR(color_for_port(port)));
                mvwprintw(main_win, y, 74, "%-6.6s", port);
                wattroff(main_win, COLOR_PAIR(color_for_port(port)));

                mvwprintw(main_win, y, 82, "%.*s", maxx - 83, desc);

                if (is_selected) wattroff(main_win, COLOR_PAIR(6) | A_BOLD);
                y++;
            }
        }

        wrefresh(main_win);

        werase(status_win);
        wattron(status_win, COLOR_PAIR(5));
        mvwprintw(status_win, 0, 0,
                  "Browse: Pfeile/jk | Enter=Details | / Filter | a=Favorit | o=Browser | q=Zurueck");
        wattroff(status_win, COLOR_PAIR(5));
        wrefresh(status_win);

        int ch = wgetch(main_win);

        if (ch == 'q' || ch == 'Q' || ch == 27) {
            break;
        } else if ((ch == KEY_UP || ch == 'k') && filtered_count > 0) {
            selected = (selected > 0) ? selected - 1 : filtered_count - 1;
        } else if ((ch == KEY_DOWN || ch == 'j') && filtered_count > 0) {
            selected = (selected < filtered_count - 1) ? selected + 1 : 0;
        } else if (ch == '/') {
            werase(status_win);
            wattron(status_win, COLOR_PAIR(5));
            mvwprintw(status_win, 0, 0, "Filter: ");
            wattroff(status_win, COLOR_PAIR(5));
            wrefresh(status_win);
            echo();
            curs_set(1);
            wgetnstr(status_win, filter, sizeof(filter) - 1);
            noecho();
            curs_set(0);
            selected = 0;
        } else if ((ch == '\n' || ch == KEY_ENTER) && filtered_count > 0) {
            service_detail_page_ui(all_services[filtered_idx[selected]]);
        } else if ((ch == 'o' || ch == 'O') && filtered_count > 0) {
            const char *svc = all_services[filtered_idx[selected]];
            char *scope = detect_scope(svc);
            char *port  = guess_port(svc, scope);
            if (port && strcmp(port, "-") != 0 && strlen(port) > 0) {
                open_in_browser_ui(port);
            } else {
                show_message_ui("Kein Port erkannt oder Service lauscht nicht.");
            }
        } else if ((ch == 'a' || ch == 'A') && filtered_count > 0) {
            const char *svc = all_services[filtered_idx[selected]];
            int exists = 0;
            for (int i = 0; i < num_my_services; i++) {
                if (strcmp(my_services[i], svc) == 0) { exists = 1; break; }
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
                invalidate_cache();
                show_message_ui("Service zu Favoriten hinzugefuegt.");
            }
        }
    }
}

// --------------------------------------------------
// Detailseite
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
        parse_summary(summary, parts);

        // Invalidate cache so detail page shows fresh state
        const char *scope_str = parts[0];
        const char *active    = parts[1];
        const char *enabled   = parts[2];
        const char *desc      = parts[3];
        const char *port      = parts[4];

        // Re-query fresh state for detail page (cache has 5s TTL)
        char fresh_active[32] = {0}, fresh_enabled[32] = {0};
        const char *user_flag = (strcmp(scope_str, "system") == 0 ? "" : "--user");
        char cmd[MAX_LINE];

        if (strcmp(scope_str, "none") != 0) {
            snprintf(cmd, sizeof(cmd), "systemctl %s is-active \"%s\" 2>/dev/null", user_flag, svc);
            execute_cmd(cmd, fresh_active, sizeof(fresh_active));
            snprintf(cmd, sizeof(cmd), "systemctl %s is-enabled \"%s\" 2>/dev/null", user_flag, svc);
            execute_cmd(cmd, fresh_enabled, sizeof(fresh_enabled));
        }

        if (strlen(fresh_active) > 0) active = fresh_active;
        if (strlen(fresh_enabled) > 0) enabled = fresh_enabled;

        const char *scope_label;
        if (strcmp(scope_str, "system") == 0)
            scope_label = "System-Service";
        else if (strcmp(scope_str, "user") == 0)
            scope_label = "User-Service (~/.config/systemd/user)";
        else
            scope_label = "NICHT GEFUNDEN";

        if (strcmp(scope_str, "none") == 0) {
            wattron(main_win, COLOR_PAIR(4) | A_BOLD);
            mvwprintw(main_win, y++, 0, "ACHTUNG: Service nicht gefunden.");
            wattroff(main_win, COLOR_PAIR(4) | A_BOLD);
            y++;
        }

        wattron(main_win, COLOR_PAIR(1));
        mvwprintw(main_win, y++, 0, "Uebersicht");
        wattroff(main_win, COLOR_PAIR(1));

        mvwprintw(main_win, y++, 0, "  Name:        %s", svc);
        mvwprintw(main_win, y++, 0, "  Scope:       %s", scope_label);

        wattron(main_win, COLOR_PAIR(color_for_active(active)));
        mvwprintw(main_win, y++, 0, "  Active:      %s", active);
        wattroff(main_win, COLOR_PAIR(color_for_active(active)));

        wattron(main_win, COLOR_PAIR(color_for_enabled(enabled)));
        mvwprintw(main_win, y++, 0, "  Enabled:     %s", enabled);
        wattroff(main_win, COLOR_PAIR(color_for_enabled(enabled)));

        wattron(main_win, COLOR_PAIR(color_for_port(port)));
        mvwprintw(main_win, y++, 0, "  Port:        %s", port);
        wattroff(main_win, COLOR_PAIR(color_for_port(port)));

        mvwprintw(main_win, y++, 0, "  Description: %s", desc);

        // Zusätzliche Infos
        char act_ts[MAX_LINE] = {0};
        char fragment_path[MAX_LINE] = {0};
        char unit_state[32] = {0};
        char substate[32] = {0};

        if (strcmp(scope_str, "none") != 0) {
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p ActiveEnterTimestamp --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, act_ts, sizeof(act_ts));

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
        mvwprintw(main_win, y++, 0, "Zusaetzliche Infos");
        wattroff(main_win, COLOR_PAIR(1));

        mvwprintw(main_win, y++, 0, "  LoadState:     %s", strlen(unit_state) ? unit_state : "unknown");
        mvwprintw(main_win, y++, 0, "  SubState:      %s", strlen(substate) ? substate : "unknown");
        mvwprintw(main_win, y++, 0, "  Fragment:      %s", strlen(fragment_path) ? fragment_path : "unknown");
        mvwprintw(main_win, y++, 0, "  Active seit:   %s", strlen(act_ts) ? act_ts : "n/a");

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
                wattron(main_win, COLOR_PAIR(5) | A_DIM);
                mvwprintw(main_win, y++, 0, "%s", log_line);
                wattroff(main_win, COLOR_PAIR(5) | A_DIM);
            }
            pclose(log_fp);
        } else {
            mvwprintw(main_win, y++, 0, "(Keine Logs verfuegbar)");
        }

        box(main_win, 0, 0);
        wrefresh(main_win);

        werase(status_win);
        wattron(status_win, COLOR_PAIR(1));
        mvwprintw(status_win, 0, 0, " s=Start | t=Stop | r=Restart | e=Enable | d=Disable | S=Status | L=Live-Logs | o=Browser | c=CPU/RAM | D=Deps | V=Edit Unit | q=Zurueck");
        wattroff(status_win, COLOR_PAIR(1));
        wrefresh(status_win);

        int ch = wgetch(status_win);

        if (ch == 'q' || ch == 'Q' || ch == 27) {
            break;
        } else if (ch == 's') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s start \"%s\"", sudo_flag, user_flag, svc);
            int ret = system(cmd);
            if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0)
                show_message_ui("Gestartet.");
            else
                show_message_ui("Start fehlgeschlagen. Siehe Logs.");
        } else if (ch == 't' || ch == 'T') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s stop \"%s\"", sudo_flag, user_flag, svc);
            int ret = system(cmd);
            if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0)
                show_message_ui("Gestoppt.");
            else
                show_message_ui("Stop fehlgeschlagen.");
        } else if (ch == 'r' || ch == 'R') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s restart \"%s\"", sudo_flag, user_flag, svc);
            int ret = system(cmd);
            if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0)
                show_message_ui("Neugestartet.");
            else
                show_message_ui("Restart fehlgeschlagen.");
        } else if (ch == 'e' || ch == 'E') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s enable \"%s\"", sudo_flag, user_flag, svc);
            system(cmd);
            invalidate_cache(); // Cache invalidieren nach enable
        } else if (ch == 'd') {
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s disable \"%s\"", sudo_flag, user_flag, svc);
            system(cmd);
            invalidate_cache();
        } else if (ch == 'S') {
            def_prog_mode();
            endwin();
            snprintf(cmd, sizeof(cmd), "systemctl %s status \"%s\" | less", user_flag, svc);
            system(cmd);
            reset_prog_mode();
            refresh();
            invalidate_cache();
        } else if (ch == 'L') {
            def_prog_mode();
            endwin();
            snprintf(cmd, sizeof(cmd), "journalctl %s -u \"%s\" -f | less", user_flag, svc);
            system(cmd);
            reset_prog_mode();
            refresh();
        } else if (ch == 'o' || ch == 'O') {
            if (strcmp(port, "-") != 0 && strlen(port) > 0) {
                open_in_browser_ui(port);
            } else {
                show_message_ui("Kein Port erkannt.");
            }
        } else if (ch == 'c' || ch == 'C') {
            char pid_str[32] = {0};
            snprintf(cmd, sizeof(cmd),
                     "systemctl %s show -p MainPID --value \"%s\" 2>/dev/null",
                     user_flag, svc);
            execute_cmd(cmd, pid_str, sizeof(pid_str));
            long pid = atol(pid_str);
            if (pid <= 0) {
                show_message_ui("PID nicht verfuegbar.");
            } else {
                float cpu = 0.0f;
                long rss  = 0;
                if (get_resource_usage(pid, &cpu, &rss) == 0) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "RAM: %ld KB  (PID: %ld)", rss, pid);
                    show_message_ui(msg);
                } else {
                    show_message_ui("Ressourcen nicht ermittelbar.");
                }
            }
        } else if (ch == 'D') {
            show_dependencies_ui(svc, scope_str);
        } else if (ch == 'V') {
            if (strcmp(scope_str, "none") == 0) {
                show_message_ui("Service nicht gefunden -- keine Unit-File.");
            } else {
                edit_unit_file_ui(svc, scope_str);
                invalidate_cache();
            }
        }
    }
    invalidate_cache(); // Nach Detailseite: Cache invalidieren
}

// --------------------------------------------------
// Add / Remove Favoriten
// --------------------------------------------------

void add_service_ui(const char *home) {
    if (!main_win || !status_win) return;

    char svc[MAX_LINE] = {0};

    werase(status_win);
    wattron(status_win, COLOR_PAIR(5));
    mvwprintw(status_win, 0, 0, "Service zu Favoriten hinzufuegen");
    mvwprintw(status_win, 1, 0, "Name: ");
    wattroff(status_win, COLOR_PAIR(5));
    wrefresh(status_win);

    echo();
    curs_set(1);
    wgetnstr(status_win, svc, sizeof(svc) - 1);
    noecho();
    curs_set(0);

    // Trim
    for (int i = strlen(svc) - 1; i >= 0 && isspace((unsigned char)svc[i]); i--)
        svc[i] = '\0';

    if (strlen(svc) == 0) {
        show_message_ui("Abgebrochen (kein Name).");
        return;
    }

    // Duplikat-Pruefung
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
        mvwprintw(status_win, 0, 0, "Service nicht gefunden.");
        mvwprintw(status_win, 1, 0, "Trotzdem hinzufuegen? (y/N): ");
        wattroff(status_win, COLOR_PAIR(3));
        wrefresh(status_win);
        int ch = wgetch(status_win);
        if (ch != 'y' && ch != 'Y') {
            show_message_ui("Nicht hinzugefuegt.");
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
    invalidate_cache();

    show_message_ui("Service hinzugefuegt.");
}

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
    mvwprintw(status_win, 0, 0, "Nummer (0 = Abbrechen): ");
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
    if (idx > num_my_services) {
        show_message_ui("Ungueltige Auswahl.");
        return;
    }

    idx--;
    char removed[MAX_LINE];
    strncpy(removed, my_services[idx], sizeof(removed) - 1);
    removed[sizeof(removed) - 1] = '\0';

    invalidate_service_cache(my_services[idx]);

    for (int i = idx; i < num_my_services - 1; i++) {
        memmove(my_services[i], my_services[i + 1], MAX_LINE);
    }
    num_my_services--;
    save_services(home);

    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Entfernt: %s", removed);
    show_message_ui(msg);
}

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

    for (int i = strlen(buf) - 1; i >= 0 && isspace((unsigned char)buf[i]); i--)
        buf[i] = '\0';

    werase(status_win);
    wrefresh(status_win);
}
