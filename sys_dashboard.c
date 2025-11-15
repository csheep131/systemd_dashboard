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

#include "sys_dashboard.h"
#include "ui.h"
#include "utils.h"

// Globale Variablen
char my_services[MAX_SERVICES][MAX_LINE];
int  num_my_services = 0;

char all_services[MAX_SERVICES][MAX_LINE];
int  num_all_services = 0;

// Default-Favoriten
const char *DEFAULT_SERVICES[DEFAULT_SERVICES_COUNT] = {
    "trainee_trainer-gunicorn.service",
    "stock-predictor.service",
    "dashboard_sheep.service"
};

// sudo-Flag (leer bei root, sonst "sudo ")
const char *sudo_flag = "";

// ---------------------------------------------------------
// sudo-Flag anhand EUID bestimmen
// ---------------------------------------------------------
void init_sudo_flag(void) {
    if (geteuid() == 0) {
        sudo_flag = "";
    } else {
        sudo_flag = "sudo ";
    }
}

// ---------------------------------------------------------
// Kommando ausführen und Ausgabe einsammeln
// ---------------------------------------------------------
int execute_cmd(const char *cmd, char *output, size_t max_output) {
    if (!output || max_output == 0) return -1;
    output[0] = '\0';

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    size_t len = 0;
    while (fgets(output + len, max_output - len, fp) != NULL && len < max_output - 1) {
        len = strlen(output);
    }
    pclose(fp);

    if (len > 0 && output[len - 1] == '\n')
        output[len - 1] = '\0';

    return 0;
}

// ---------------------------------------------------------
// Kommando ohne Ausgabe (Exitcode)
// ---------------------------------------------------------
int exec_simple(const char *cmd) {
    int ret = system(cmd);
    if (WIFEXITED(ret)) {
        return WEXITSTATUS(ret);
    }
    return -1;
}

// ---------------------------------------------------------
// systemctl vorhanden?
// ---------------------------------------------------------
void check_systemctl(void) {
    if (exec_simple("which systemctl >/dev/null 2>&1") != 0) {
        fprintf(stderr, "%sFehler:%s 'systemctl' wurde nicht gefunden. Läuft hier kein systemd?\n",
                ERR_COLOR, RESET_COLOR);
        exit(1);
    }
}

// ---------------------------------------------------------
// Farbige Status-Strings (via ANSI-Codes)
// ---------------------------------------------------------
void status_color(const char *s, char *buf, size_t bufsize) {
    if (!s || !buf) return;

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
    if (!s || !buf) return;

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

// ---------------------------------------------------------
// Services laden / speichern
// ---------------------------------------------------------
void load_services(const char *home) {
    char config_path[MAX_LINE];
    snprintf(config_path, sizeof(config_path), CONFIG_FILE, home);

    num_my_services = 0;

    FILE *fp = fopen(config_path, "r");
    if (fp) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = '\0';
            if (strlen(line) == 0 || line[0] == '#') continue;
            if (num_my_services < MAX_SERVICES) {
                strncpy(my_services[num_my_services++], line, MAX_LINE - 1);
                my_services[num_my_services - 1][MAX_LINE - 1] = '\0';
            }
        }
        fclose(fp);
    } else {
        // Defaults beim ersten Start
        for (int i = 0; i < DEFAULT_SERVICES_COUNT && num_my_services < MAX_SERVICES; i++) {
            strncpy(my_services[num_my_services++], DEFAULT_SERVICES[i], MAX_LINE - 1);
            my_services[num_my_services - 1][MAX_LINE - 1] = '\0';
        }
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

    for (int i = 0; i < num_my_services; i++) {
        fprintf(fp, "%s\n", my_services[i]);
    }

    fclose(fp);
}

// ---------------------------------------------------------
// Scope (system/user/none) erkennen
// ---------------------------------------------------------
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

// ---------------------------------------------------------
// Port raten – erst ExecStart/Env, dann netstat+PID, dann ss+PID
// ---------------------------------------------------------
char *guess_port(const char *svc, const char *scope) {
    static char port_buf[16];
    strcpy(port_buf, "-");

    if (!svc || !scope || strcmp(scope, "none") == 0)
        return port_buf;

    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");

    char cmd[MAX_LINE];
    char exec_out[MAX_LINE] = {0};
    char env_out[MAX_LINE]  = {0};

    // 1) ExecStart & Environment durchsuchen (statischer Guess)
    snprintf(cmd, sizeof(cmd),
             "systemctl %s show -p ExecStart --value \"%s\" 2>/dev/null",
             user_flag, svc);
    execute_cmd(cmd, exec_out, sizeof(exec_out));

    snprintf(cmd, sizeof(cmd),
             "systemctl %s show -p Environment --value \"%s\" 2>/dev/null",
             user_flag, svc);
    execute_cmd(cmd, env_out, sizeof(env_out));

    // Anführungszeichen aus ExecStart entfernen
    char *q = strchr(exec_out, '"');
    while (q) {
        memmove(q, q + 1, strlen(q));
        q = strchr(q, '"');
    }

    // einfache Muster: --port=XXXX / --port XXXX
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
            // KEIN return hier – netstat/ss darf das noch überschreiben
        }
    }

    // -p 8080
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
        }
    }

    // letzte :Zahl (z.B. 0.0.0.0:8000)
    p = strrchr(exec_out, ':');
    if (p && isdigit((unsigned char)*(p + 1))) {
        p++;
        char *end = p;
        while (isdigit((unsigned char)*end)) end++;
        int len = end - p;
        if (len > 0 && len < 6) {
            strncpy(port_buf, p, len);
            port_buf[len] = '\0';
        }
    }

    // Environment: PORT=XXXX
    p = strstr(env_out, "PORT=");
    if (p) {
        p += 5;
        char *end = p;
        while (isdigit((unsigned char)*end)) end++;
        int len = end - p;
        if (len > 0 && len < 6) {
            strncpy(port_buf, p, len);
            port_buf[len] = '\0';
        }
    }

    // 2) Fallback / Override: MainPID holen, dann mit netstat/ss Ports ermitteln
    char pid_str[32] = {0};
    snprintf(cmd, sizeof(cmd),
             "systemctl %s show -p MainPID --value \"%s\" 2>/dev/null",
             user_flag, svc);
    if (execute_cmd(cmd, pid_str, sizeof(pid_str)) == 0 && strlen(pid_str) > 0) {
        long pid = atol(pid_str);
        if (pid > 0) {
            char net_out[MAX_LINE * 10] = {0};

            // netstat zuerst
            snprintf(cmd, sizeof(cmd),
                     "netstat -tulnp 2>/dev/null | grep ' %ld/'",
                     pid);
            execute_cmd(cmd, net_out, sizeof(net_out));

            // Wenn netstat nichts bringt, ss probieren
            if (strlen(net_out) == 0) {
                snprintf(cmd, sizeof(cmd),
                         "ss -tulnp 2>/dev/null | grep 'pid=%ld,'",
                         pid);
                execute_cmd(cmd, net_out, sizeof(net_out));
            }

            if (strlen(net_out) > 0) {
                // letztes ':' gefolgt von Ziffern als Port
                char *scan = net_out;
                char *last_port_start = NULL;

                while ((scan = strchr(scan, ':')) != NULL) {
                    if (isdigit((unsigned char)scan[1])) {
                        last_port_start = scan + 1;
                    }
                    scan++;
                }

                if (last_port_start) {
                    char *end = last_port_start;
                    while (isdigit((unsigned char)*end)) end++;
                    int len = end - last_port_start;
                    if (len > 0 && len < 6) {
                        // HIER überschreiben wir ggf. den statischen Guess
                        strncpy(port_buf, last_port_start, len);
                        port_buf[len] = '\0';
                    }
                }
            }
        }
    }

    return port_buf;
}

// ---------------------------------------------------------
// Kurz-Info über Service: scope|active|enabled|desc|port
// ---------------------------------------------------------
void get_service_summary(const char *svc, char *summary, size_t bufsize) {
    char scope_str[16];
    strcpy(scope_str, detect_scope(svc));

    char active[32]  = {0};
    char enabled[32] = {0};
    char desc[MAX_DESC] = {0};
    char port[16]    = {0};

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

    if (strlen(desc) == 0)
        strcpy(desc, "(keine Beschreibung)");

    strcpy(port, guess_port(svc, scope_str));

    snprintf(summary, bufsize, "%s|%s|%s|%s|%s",
             scope_str, active, enabled, desc, port);
}

// ---------------------------------------------------------
// Liste aller Services (system + user)
// ---------------------------------------------------------
void build_all_services_list(const char *home) {
    num_all_services = 0;

    // System-Units
    DIR *dir = opendir("/etc/systemd/system");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            if (entry->d_type == DT_REG &&
                strstr(entry->d_name, ".service") != NULL) {
                strncpy(all_services[num_all_services++],
                        entry->d_name, MAX_LINE - 1);
                all_services[num_all_services - 1][MAX_LINE - 1] = '\0';
            }
        }
        closedir(dir);
    }

    // User-Units (~/.config/systemd/user)
    char user_dir[MAX_LINE];
    snprintf(user_dir, sizeof(user_dir),
             "%s/.config/systemd/user", home);

    dir = opendir(user_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            if (entry->d_type == DT_REG &&
                strstr(entry->d_name, ".service") != NULL) {

                int exists = 0;
                for (int i = 0; i < num_all_services; i++) {
                    if (strcmp(all_services[i], entry->d_name) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists) {
                    strncpy(all_services[num_all_services++],
                            entry->d_name, MAX_LINE - 1);
                    all_services[num_all_services - 1][MAX_LINE - 1] = '\0';
                }
            }
        }
        closedir(dir);
    }
}

// ---------------------------------------------------------
// Hauptloop (ncurses)
// ---------------------------------------------------------
static void main_loop(const char *home) {
    init_ui();

    for (;;) {
        render_dashboard_ui();

        char input[MAX_LINE] = {0};
        get_input(input, sizeof(input));
        if (input[0] == '\0')
            continue;

        char c = input[0];

        if (c == 'q' || c == 'Q') {
            end_ui();
            printf("\n%sBye%s\n", DIM_COLOR, RESET_COLOR);
            exit(0);
        } else if (c == 'a' || c == 'A') {
            add_service_ui(home);
        } else if (c == 'r') {       // remove
            remove_service_ui(home);
        } else if (c == 'R') {       // reload config
            load_services(home);
            show_message_ui("Favoriten-Konfiguration neu geladen.");
        } else if (c == 'b' || c == 'B') {
            browse_all_services_ui(home);
        } else {
            // Zahl → Detailansicht Favoriten
            int idx = atoi(input);
            if (idx >= 1 && idx <= num_my_services) {
                service_detail_page_ui(my_services[idx - 1]);
            } else {
                show_message_ui("Ungültige Auswahl.");
            }
        }
    }
}

// ---------------------------------------------------------
// main
// ---------------------------------------------------------
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

    return 0;
}
