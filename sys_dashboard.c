#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
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

const char *DEFAULT_SERVICES[DEFAULT_SERVICES_COUNT] = {
    "trainee_trainer-gunicorn.service",
    "stock-predictor.service",
    "dashboard_sheep.service"
};

const char *sudo_flag = "";

// --------------------------------------------------
// Helper
// --------------------------------------------------

void init_sudo_flag(void) {
    if (geteuid() == 0)
        sudo_flag = "";
    else
        sudo_flag = "sudo ";
}

int execute_cmd(const char *cmd, char *output, size_t max_output) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    size_t len = 0;
    output[0] = '\0';

    while (fgets(output + len, max_output - len, fp) != NULL &&
           len < max_output - 1) {
        len = strlen(output);
    }
    pclose(fp);
    if (len > 0 && output[len - 1] == '\n') {
        output[len - 1] = '\0';
    }
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
        printf("%sFehler:%s 'systemctl' wurde nicht gefunden. Läuft hier kein systemd?\n",
               ERR_COLOR, RESET_COLOR);
        exit(1);
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
                strncpy(my_services[num_my_services++], line, MAX_LINE - 1);
                my_services[num_my_services-1][MAX_LINE-1] = '\0';
            }
        }
        fclose(fp);
    } else {
        // Defaults
        for (int i = 0; i < DEFAULT_SERVICES_COUNT && num_my_services < MAX_SERVICES; i++) {
            strncpy(my_services[num_my_services++], DEFAULT_SERVICES[i], MAX_LINE-1);
            my_services[num_my_services-1][MAX_LINE-1] = '\0';
        }
        // direkt speichern
        save_services(home);
    }
}

void save_services(const char *home) {
    char config_path[MAX_LINE];
    snprintf(config_path, sizeof(config_path), CONFIG_FILE, home);

    FILE *fp = fopen(config_path, "w");
    if (!fp) return;

    fprintf(fp, "# Eigene systemd-Services für das Dashboard\n");
    fprintf(fp, "# Eine Unit pro Zeile, z.B.:\n");
    fprintf(fp, "# trainee_trainer-gunicorn.service\n\n");

    for (int i = 0; i < num_my_services; ++i) {
        fprintf(fp, "%s\n", my_services[i]);
    }
    fclose(fp);
}

// --------------------------------------------------
// Farben (CLI, nicht ncurses) – für alte Pfade
// --------------------------------------------------

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
// Scope + Port-Erkennung
// --------------------------------------------------

char *detect_scope(const char *svc) {
    static char scope_buf[16];
    char cmd[MAX_LINE];
    char out[MAX_LINE];

    out[0] = '\0';

    /* ==========================
       1) System-Scope testen
       ========================== */
    snprintf(cmd, sizeof(cmd),
             "systemctl list-unit-files --type=service \"%s\" 2>/dev/null",
             svc);
    execute_cmd(cmd, out, sizeof(out));

    // Wenn der Servicename in der Ausgabe vorkommt, zählen wir ihn als System-Unit
    if (strstr(out, svc) != NULL) {
        strcpy(scope_buf, "system");
        return scope_buf;
    }

    /* ==========================
       2) User-Scope testen
       ========================== */
    out[0] = '\0';
    snprintf(cmd, sizeof(cmd),
             "systemctl --user list-unit-files --type=service \"%s\" 2>/dev/null",
             svc);
    execute_cmd(cmd, out, sizeof(out));

    if (strstr(out, svc) != NULL) {
        strcpy(scope_buf, "user");
        return scope_buf;
    }

    /* ==========================
       3) Weder system noch user
       ========================== */
    strcpy(scope_buf, "none");
    return scope_buf;
}

char *guess_port(const char *svc, const char *scope) {
    static char port_buf[16];
    strcpy(port_buf, "-");

    // Wenn Service nicht gefunden → kein Port
    if (strcmp(scope, "none") == 0) {
        return port_buf;
    }

    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");

    // 1) MainPID vom Service holen
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


    //
    // netstat -tulnp | awk '$0 ~ "PID/" {split($4,a,":"); print a[length(a)]; exit}'
    //
    char net_out[64] = {0};
    snprintf(cmd, sizeof(cmd),
             "netstat -tulnp 2>/dev/null | "
             "awk '$0 ~ \"%ld/\" {split($4,a,\":\"); print a[length(a)]; exit}'",
             pid);

    if (execute_cmd(cmd, net_out, sizeof(net_out)) == 0 && strlen(net_out) > 0) {
        // Sicherheitshalber prüfen, ob das Ergebnis nur aus Ziffern besteht
        int ok = 1;
        for (size_t i = 0; i < strlen(net_out); i++) {
            if (!isdigit((unsigned char)net_out[i])) {
                ok = 0;
                break;
            }
        }
        if (ok && strlen(net_out) < sizeof(port_buf)) {
            strcpy(port_buf, net_out);
        }
    }

    return port_buf;
}


// --------------------------------------------------
// Service-Summary (Performance: Statischer Cache-Buffer für wiederholte Aufrufe)
// --------------------------------------------------
static char summary_cache[MAX_SERVICES][MAX_LINE] = {0};  // Einfacher Cache
static int cache_valid[MAX_SERVICES] = {0};

void get_service_summary(const char *svc, char *summary, size_t bufsize) {
    // Cache-Check (einfach: Index über String-Hash, aber für Einfachheit: Linear-Suche)
    for (int i = 0; i < num_my_services; i++) {
        if (strcmp(my_services[i], svc) == 0 && cache_valid[i]) {
            strncpy(summary, summary_cache[i], bufsize - 1);
            summary[bufsize - 1] = '\0';
            return;
        }
    }

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

    // Cache speichern
    for (int i = 0; i < num_my_services; i++) {
        if (strcmp(my_services[i], svc) == 0) {
            strncpy(summary_cache[i], summary, MAX_LINE - 1);
            summary_cache[i][MAX_LINE - 1] = '\0';
            cache_valid[i] = 1;
            break;
        }
    }
}

// --------------------------------------------------
// Alle Services einsammeln (System + User)
// --------------------------------------------------
void build_all_services_list(const char *home) {
    num_all_services = 0;

    // System services from /etc/systemd/system/*.service
    DIR *dir = opendir("/etc/systemd/system");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            // nur Einträge mit ".service" im Namen
            if (strstr(entry->d_name, ".service") != NULL) {
                strcpy(all_services[num_all_services++], entry->d_name);
            }
        }
        closedir(dir);
    }

    // User services from ~/.config/systemd/user/*.service
    char user_dir[MAX_LINE];
    snprintf(user_dir, sizeof(user_dir), "%s/.config/systemd/user", home);
    dir = opendir(user_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            if (strstr(entry->d_name, ".service") != NULL) {
                // Check if already added (user services might overlap)
                int exists = 0;
                for (int i = 0; i < num_all_services; i++) {
                    if (strcmp(all_services[i], entry->d_name) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists) {
                    strcpy(all_services[num_all_services++], entry->d_name);
                }
            }
        }
        closedir(dir);
    }
}

// --------------------------------------------------
// Alte CLI-Dialoge (werden via Wrapper aus ncurses genutzt)
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
    print_header("Service zu Favoriten hinzufügen");
    printf("Gib den exakten Servicenamen ein (z.B. trainee_trainer-gunicorn.service):\n> ");

    char svc[MAX_LINE];
    if (!fgets(svc, sizeof(svc), stdin)) return;
    svc[strcspn(svc, "\n")] = '\0';

    if (svc[0] == '\0') {
        printf("%sAbgebrochen (kein Name).%s\n", DIM_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    for (int i = 0; i < num_my_services; ++i) {
        if (strcmp(my_services[i], svc) == 0) {
            printf("%sService ist bereits in der Favoritenliste.%s\n",
                   WARN_COLOR, RESET_COLOR);
            press_enter();
            return;
        }
    }

    char *scope = detect_scope(svc);
    if (strcmp(scope, "none") == 0) {
        printf("%sHinweis:%s Service wird aktuell weder als System- noch als User-Service gefunden.\n",
               WARN_COLOR, RESET_COLOR);
        printf("Trotzdem zu Favoriten hinzufügen? [y/N] ");
        char ans[8];
        if (!fgets(ans, sizeof(ans), stdin)) return;
        if (tolower(ans[0]) != 'y') {
            printf("%sNicht hinzugefügt.%s\n", DIM_COLOR, RESET_COLOR);
            press_enter();
            return;
        }
    } else {
        printf("%sErkannt als %s-Service.%s\n", OK_COLOR, scope, RESET_COLOR);
    }

    if (num_my_services < MAX_SERVICES) {
        strncpy(my_services[num_my_services++], svc, MAX_LINE-1);
        my_services[num_my_services-1][MAX_LINE-1] = '\0';
        save_services(home);
        printf("%sService zu Favoriten hinzugefügt.%s\n", OK_COLOR, RESET_COLOR);
    } else {
        printf("%sMaximale Anzahl Services erreicht.%s\n", ERR_COLOR, RESET_COLOR);
    }
    press_enter();
}

void remove_service_interactive(const char *home) {
    print_header("Service aus Favoriten entfernen");

    if (num_my_services == 0) {
        printf("%sKeine Favoriten vorhanden.%s\n", DIM_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    for (int i = 0; i < num_my_services; ++i) {
        printf("  %d) %s\n", i + 1, my_services[i]);
    }

    printf("\nNummer zum Entfernen (oder leer für Abbruch): ");
    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) return;
    buf[strcspn(buf, "\n")] = '\0';
    if (buf[0] == '\0') {
        printf("%sAbgebrochen.%s\n", DIM_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    int idx = atoi(buf);
    if (idx < 1 || idx > num_my_services) {
        printf("%sUngültige Auswahl.%s\n", ERR_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    char removed[MAX_LINE];
    strncpy(removed, my_services[idx-1], MAX_LINE-1);
    removed[MAX_LINE-1] = '\0';

    for (int i = idx-1; i < num_my_services - 1; ++i) {
        strcpy(my_services[i], my_services[i+1]);
    }
    num_my_services--;
    save_services(home);

    printf("%sService entfernt:%s %s\n", OK_COLOR, RESET_COLOR, removed);
    press_enter();
}

void main_loop(const char *home) {
    init_ui();

    int selected = 0;        // Index des ausgewählten Services
    int focus_on_list = 1;   // 1 = Fokus auf Liste

    // Gleich beim Start einmal zeichnen
    render_dashboard_ui(selected, focus_on_list);

    while (1) {

        if (num_my_services <= 0) {
            selected = 0;
        } else if (selected >= num_my_services) {
            selected = num_my_services - 1;
        }

        // Taste lesen NACHDEM die UI gezeichnet wurde
        int ch = getch();

        // Beenden (nur auf Hauptseite)
        if (ch == 'q' || ch == 'Q') {
            end_ui();
            printf("\n%sBye%s\n", DIM_COLOR, RESET_COLOR);
            exit(0);
        }
        // Fokus wechseln (Tab)
        else if (ch == '\t') {
            focus_on_list = !focus_on_list;
        }
        // Navigation in der Liste
        else if (focus_on_list && num_my_services > 0 && (ch == KEY_UP || ch == 'k')) {
            if (selected > 0) selected--;
            else selected = num_my_services - 1;
        }
        else if (focus_on_list && num_my_services > 0 && (ch == KEY_DOWN || ch == 'j')) {
            if (selected < num_my_services - 1) selected++;
            else selected = 0;
        }
        // Enter → Detailseite
        else if ((ch == '\n' || ch == KEY_ENTER) && focus_on_list && num_my_services > 0) {
            service_detail_page_ui(my_services[selected]);
        }
        // a → Service hinzufügen
        else if (ch == 'a' || ch == 'A') {
            add_service_ui(home);
        }
        // x → Service entfernen (geändert von r)
        else if (ch == 'x' || ch == 'X') {
            remove_service_ui(home);
        }
        // R → Config neu laden
        else if (ch == 'R') {
            load_services(home);
            // Cache invalidieren
            memset(cache_valid, 0, sizeof(cache_valid));
        }
        // B → Alle Services browsen
        else if (ch == 'B' || ch == 'b') {
            browse_all_services_ui(home);
        }
        // r → Restart selected Service (neu auf Dashboard)
        else if ((ch == 'r' || ch == 'R') && focus_on_list && num_my_services > 0) {
            const char *svc = my_services[selected];
            char *scope = detect_scope(svc);
            const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");
            char cmd[MAX_LINE];
            snprintf(cmd, sizeof(cmd), "%ssystemctl %s restart \"%s\"",
                     sudo_flag, user_flag, svc);
            system(cmd);
            show_message_ui("Service neugestartet");
            // Cache invalidieren für diesen Service
            for (int i = 0; i < num_my_services; i++) {
                if (strcmp(my_services[i], svc) == 0) {
                    cache_valid[i] = 0;
                    break;
                }
            }
        }
        // o → Browser direkt aus Dashboard
        else if (ch == 'o' || ch == 'O') {
            if (focus_on_list && num_my_services > 0) {
                const char *svc = my_services[selected];
                char *scope = detect_scope(svc);
                char *port  = guess_port(svc, scope);

                if (port && strcmp(port, "-") != 0 && strlen(port) > 0) {
                    open_in_browser(port);
                } else {
                    show_message_ui("Kein Port erkannt oder Service lauscht nicht.");
                }
            } else {
                show_message_ui("Kein Service ausgewählt.");
            }
        }

        // nach Änderung wieder neu zeichnen
        render_dashboard_ui(selected, focus_on_list);
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
        fprintf(stderr, "HOME not set\n");
        return 1;
    }

    load_services(home);
    main_loop(home);
    end_ui();

    printf("\n%sBye%s\n", DIM_COLOR, RESET_COLOR);
    return 0;
}