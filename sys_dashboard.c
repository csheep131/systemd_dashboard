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

void press_enter(void) {
    printf("\n%sWeiter mit [Enter]...%s\n", DIM_COLOR, RESET_COLOR);
    char dummy[8];
    fgets(dummy, sizeof(dummy), stdin);
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

    snprintf(cmd, sizeof(cmd), "systemctl status \"%s\" >/dev/null 2>&1", svc);
    if (exec_simple(cmd) == 0) {
        strcpy(scope_buf, "system");
        return scope_buf;
    }
    snprintf(cmd, sizeof(cmd), "systemctl --user status \"%s\" >/dev/null 2>&1", svc);
    if (exec_simple(cmd) == 0) {
        strcpy(scope_buf, "user");
        return scope_buf;
    }
    strcpy(scope_buf, "none");
    return scope_buf;
}

// Port-Erkennung: zuerst ExecStart/Env, dann netstat mit PID
char *guess_port(const char *svc, const char *scope) {
    static char port_buf[16];
    strcpy(port_buf, "-");

    if (strcmp(scope, "none") == 0) return port_buf;

    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");
    char cmd[MAX_LINE];
    char exec_out[MAX_LINE] = {0};
    char env_out[MAX_LINE]  = {0};

    // ExecStart auslesen
    snprintf(cmd, sizeof(cmd),
             "systemctl %s show -p ExecStart --value \"%s\" 2>/dev/null",
             user_flag, svc);
    execute_cmd(cmd, exec_out, sizeof(exec_out));

    // Environment auslesen
    snprintf(cmd, sizeof(cmd),
             "systemctl %s show -p Environment --value \"%s\" 2>/dev/null",
             user_flag, svc);
    execute_cmd(cmd, env_out, sizeof(env_out));

    // Quotes entfernen
    char *q = strchr(exec_out, '"');
    while (q) {
        memmove(q, q + 1, strlen(q));
        q = strchr(q, '"');
    }

    // 1) --port= / --port
    char *p = strstr(exec_out, "--port=");
    if (!p) p = strstr(exec_out, "--port ");
    if (p) {
        if (strstr(p, "--port="))
            p = strchr(p, '=') + 1;
        else
            p = strstr(p, "port ") + 5;
        while (*p == ' ') p++;
        char *end = p;
        while (isdigit((unsigned char)*end)) end++;
        int len = end - p;
        if (len > 0 && len < 6) {
            strncpy(port_buf, p, len);
            port_buf[len] = '\0';
            return port_buf;
        }
    }

    // 2) -p <port>
    p = strstr(exec_out, "-p ");
    if (p) {
        p += 2;
        while (*p == ' ') p++;
        char *end = p;
        while (isdigit((unsigned char)*end)) end++;
        int len = end - p;
        if (len > 0 && len < 6) {
            strncpy(port_buf, p, len);
            port_buf[len] = '\0';
            return port_buf;
        }
    }

    // 3) letzte :<digits>
    p = strrchr(exec_out, ':');
    if (p && isdigit((unsigned char)*(p + 1))) {
        p++;
        char *end = p;
        while (isdigit((unsigned char)*end)) end++;
        int len = end - p;
        if (len > 0 && len < 6) {
            strncpy(port_buf, p, len);
            port_buf[len] = '\0';
            return port_buf;
        }
    }

    // 4) Environment=...PORT=xxxx...
    p = strstr(env_out, "PORT=");
    if (p) {
        p += 5;
        char *end = p;
        while (isdigit((unsigned char)*end)) end++;
        int len = end - p;
        if (len > 0 && len < 6) {
            strncpy(port_buf, p, len);
            port_buf[len] = '\0';
            return port_buf;
        }
    }

    // 5) Fallback: netstat -tulnp + PID
    char pid_str[32] = {0};
    snprintf(cmd, sizeof(cmd),
             "systemctl %s show -p MainPID --value \"%s\" 2>/dev/null",
             user_flag, svc);
    if (execute_cmd(cmd, pid_str, sizeof(pid_str)) == 0 && strlen(pid_str) > 0) {
        long pid = atol(pid_str);
        if (pid > 0) {
            char ns_out[MAX_LINE * 10] = {0};
            // entspricht deinem Beispiel: netstat ... | grep 52405
            snprintf(cmd, sizeof(cmd),
                     "netstat -tulnp 2>/dev/null | grep ' %ld/'", pid);
            if (execute_cmd(cmd, ns_out, sizeof(ns_out)) == 0 &&
                strlen(ns_out) > 0) {

                // Suche nach ":" gefolgt von Ziffern → das ist der Port
                char *pp = strchr(ns_out, ':');
                while (pp) {
                    if (isdigit((unsigned char)*(pp + 1))) {
                        pp++;
                        char *end = pp;
                        while (isdigit((unsigned char)*end)) end++;
                        int len = end - pp;
                        if (len > 0 && len < 6) {
                            strncpy(port_buf, pp, len);
                            port_buf[len] = '\0';
                            return port_buf;
                        }
                    }
                    pp = strchr(pp + 1, ':');
                }
            }
        }
    }

    return port_buf;
}

// --------------------------------------------------
// Service-Summary
// --------------------------------------------------

void get_service_summary(const char *svc, char *summary, size_t bufsize) {
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
}

// --------------------------------------------------
// Alle Services einsammeln (System + User)
// --------------------------------------------------

void build_all_services_list(const char *home) {
    num_all_services = 0;

    // /etc/systemd/system
    DIR *dir = opendir("/etc/systemd/system");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            if (entry->d_type == DT_REG &&
                strstr(entry->d_name, ".service") != NULL) {
                strncpy(all_services[num_all_services++], entry->d_name, MAX_LINE-1);
                all_services[num_all_services-1][MAX_LINE-1] = '\0';
            }
        }
        closedir(dir);
    }

    // ~/.config/systemd/user
    char user_dir[MAX_LINE];
    snprintf(user_dir, sizeof(user_dir), "%s/.config/systemd/user", home);
    dir = opendir(user_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            if (entry->d_type == DT_REG &&
                strstr(entry->d_name, ".service") != NULL) {
                int exists = 0;
                for (int i = 0; i < num_all_services; ++i) {
                    if (strcmp(all_services[i], entry->d_name) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists) {
                    strncpy(all_services[num_all_services++], entry->d_name, MAX_LINE-1);
                    all_services[num_all_services-1][MAX_LINE-1] = '\0';
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

// --------------------------------------------------
// ncurses-Hauptloop – nur noch Key-Events
// --------------------------------------------------

void main_loop(const char *home) {
    int selected = 0;

    while (1) {
        if (num_my_services <= 0)
            selected = -1;
        else if (selected >= num_my_services)
            selected = num_my_services - 1;

        render_dashboard_ui(selected, 1);
        int ch = getch();

        if (ch == 'q' || ch == 'Q') {
            // zurück nach main
            return;
        } else if (ch == KEY_UP) {
            if (selected > 0) selected--;
        } else if (ch == KEY_DOWN) {
            if (selected < num_my_services - 1) selected++;
        } else if (ch == 10 || ch == KEY_ENTER) {
            if (selected >= 0 && selected < num_my_services) {
                service_detail_page_ui(my_services[selected]);
            }
        } else if (ch == 'a' || ch == 'A') {
            add_service_ui(home);
        } else if (ch == 'r') {
            remove_service_ui(home);
        } else if (ch == 'R') {
            load_services(home);   // Config neu laden
        } else if (ch == 'b' || ch == 'B') {
            browse_all_services_ui(home);
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
        fprintf(stderr, "HOME not set\n");
        return 1;
    }

    load_services(home);
    init_ui();
    main_loop(home);
    end_ui();

    printf("\n%sBye%s\n", DIM_COLOR, RESET_COLOR);
    return 0;
}
