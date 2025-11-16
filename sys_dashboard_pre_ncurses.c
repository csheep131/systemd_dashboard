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

#define MAX_SERVICES 1000
#define MAX_LINE 1024
#define MAX_DESC 256
#define CONFIG_FILE "%s/.my_systemd_dashboard_services"
#define DEFAULT_SERVICES_COUNT 3
const char *DEFAULT_SERVICES[DEFAULT_SERVICES_COUNT] = {
    "trainee_trainer-gunicorn.service",
    "stock-predictor.service",
    "dashboard_sheep.service"
};
#define DETAIL_LOG_LINES 20

// ANSI color codes
#define HEADER_COLOR "\033[36m"  // cyan
#define OK_COLOR "\033[32m"      // green
#define WARN_COLOR "\033[33m"    // yellow
#define ERR_COLOR "\033[31m"     // red
#define DIM_COLOR "\033[90m"     // grey
#define RESET_COLOR "\033[0m"

// Global variables
char my_services[MAX_SERVICES][MAX_LINE];
int num_my_services = 0;
char all_services[MAX_SERVICES][MAX_LINE];
int num_all_services = 0;

// Function prototypes
void check_systemctl(void);
void load_services(const char *home);
void save_services(const char *home);
void print_header(const char *title);
void status_color(const char *s, char *buf, size_t bufsize);
void enabled_color(const char *s, char *buf, size_t bufsize);
void press_enter(void);
char *detect_scope(const char *svc);
char *guess_port(const char *svc, const char *scope);
void get_service_summary(const char *svc, char *summary, size_t bufsize);
void render_dashboard(void);
void add_service_interactive(const char *home);
void remove_service_interactive(const char *home);
void build_all_services_list(const char *home);
void render_all_services(void);
void browse_all_services(void);
void service_detail_page(const char *svc);
void main_loop(const char *home);

// Helper to execute command and get output
int execute_cmd(const char *cmd, char *output, size_t max_output) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    size_t len = 0;
    while (fgets(output + len, max_output - len, fp) != NULL && len < max_output - 1) {
        len = strlen(output);
    }
    pclose(fp);
    if (len > 0 && output[len - 1] == '\n') output[len - 1] = '\0';
    return 0;
}

// Helper to execute command without output (just check success)
int exec_simple(const char *cmd) {
    int ret = system(cmd);
    if (WIFEXITED(ret)) {
        return WEXITSTATUS(ret);
    }
    return -1;
}

void check_systemctl(void) {
    if (exec_simple("which systemctl >/dev/null 2>&1") != 0) {
        printf("%sFehler:%s 'systemctl' wurde nicht gefunden. Läuft hier kein systemd?\n", ERR_COLOR, RESET_COLOR);
        exit(1);
    }
}

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
                strcpy(my_services[num_my_services++], line);
            }
        }
        fclose(fp);
    } else {
        // Load defaults
        for (int i = 0; i < DEFAULT_SERVICES_COUNT && num_my_services < MAX_SERVICES; i++) {
            strcpy(my_services[num_my_services++], DEFAULT_SERVICES[i]);
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
    fprintf(fp, "# trainee_trainer-gunicorn.service\n");
    fprintf(fp, "\n");
    for (int i = 0; i < num_my_services; i++) {
        fprintf(fp, "%s\n", my_services[i]);
    }
    fclose(fp);
}

void print_header(const char *title) {
    system("clear");
    printf("%s=====================================================%s\n", HEADER_COLOR, RESET_COLOR);
    printf("%s        Systemd Dashboard – Eigene Services          %s\n", HEADER_COLOR, RESET_COLOR);
    printf("%s=====================================================%s\n", HEADER_COLOR, RESET_COLOR);
    if (title && strlen(title) > 0) {
        printf("%s%s%s\n", HEADER_COLOR, title, RESET_COLOR);
    }
    printf("\n");
}

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

void press_enter(void) {
    printf("\n");
    printf("%sWeiter mit [Enter]...%s\n", DIM_COLOR, RESET_COLOR);
    char dummy[2];
    fgets(dummy, sizeof(dummy), stdin);
}

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

char *guess_port(const char *svc, const char *scope) {
    static char port_buf[16];
    strcpy(port_buf, "-");
    if (strcmp(scope, "none") == 0) return port_buf;

    const char *user_flag = (strcmp(scope, "system") == 0 ? "" : "--user");

    // First, try static guess from ExecStart/Environment
    char cmd[MAX_LINE];
    char exec_out[MAX_LINE] = {0};
    char env_out[MAX_LINE] = {0};

    snprintf(cmd, sizeof(cmd), "systemctl %s show -p ExecStart --value \"%s\" 2>/dev/null", user_flag, svc);
    execute_cmd(cmd, exec_out, sizeof(exec_out));

    snprintf(cmd, sizeof(cmd), "systemctl %s show -p Environment --value \"%s\" 2>/dev/null", user_flag, svc);
    execute_cmd(cmd, env_out, sizeof(env_out));

    // Remove quotes from exec_out
    char *q = strchr(exec_out, '"');
    while (q) {
        memmove(q, q + 1, strlen(q));
        q = strchr(q, '"');
    }

    // Simple parsing for port from exec
    char *p = strstr(exec_out, "--port=");
    if (!p) p = strstr(exec_out, "--port ");
    if (p) {
        if (strstr(p, "--port=")) {
            p = strstr(p, "=") + 1;
        } else {
            p = strstr(p, "port ") + 5;
        }
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

    // Fallback: Dynamic port detection via PID and ss
    char pid_str[32] = {0};
    snprintf(cmd, sizeof(cmd), "systemctl %s show -p MainPID --value \"%s\" 2>/dev/null", user_flag, svc);
    if (execute_cmd(cmd, pid_str, sizeof(pid_str)) == 0 && strlen(pid_str) > 0) {
        long pid = atol(pid_str);
        if (pid > 0) {
            char ss_out[MAX_LINE * 10] = {0};  // Larger buffer for ss output
            snprintf(cmd, sizeof(cmd), "ss -tulnp 2>/dev/null | grep 'pid=%ld,'", pid);
            if (execute_cmd(cmd, ss_out, sizeof(ss_out)) == 0 && strlen(ss_out) > 0) {
                // Parse ss output for ports, e.g., *:80 or 0.0.0.0:3000
                // Look for : followed by digits
                p = strstr(ss_out, ":");
                while (p) {
                    if (isdigit((unsigned char)*(p + 1))) {
                        p++;
                        char *end = p;
                        while (isdigit((unsigned char)*end)) end++;
                        int len = end - p;
                        if (len > 0 && len < 6) {
                            strncpy(port_buf, p, len);
                            port_buf[len] = '\0';
                            return port_buf;  // Return first found port
                        }
                    }
                    p = strstr(p + 1, ":");
                }
            }
        }
    }

    return port_buf;
}

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

    snprintf(summary, bufsize, "%s|%s|%s|%s|%s", scope_str, active, enabled, desc, port);
}

void render_dashboard(void) {
    print_header("Dashboard – Favoriten");

    if (num_my_services == 0) {
        printf("%sKeine Services konfiguriert.%s\n", ERR_COLOR, RESET_COLOR);
        printf("Mit 'a' kannst du Services hinzufügen.\n");
        printf("\n");
    } else {
        printf("%-4s %-5s %-40s %-14s %-14s %-8s %-s\n", "Nr.", "SCOPE", "SERVICE", "ACTIVE", "ENABLED", "PORT", "DESCRIPTION");
        printf("%-4s %-5s %-40s %-14s %-14s %-8s %-s\n", "----", "-----", "-------", "------", "-------", "----", "-----------");

        char summary[MAX_LINE];
        char active_c[64], enabled_c[64], scope_disp[16];
        char parts[5][MAX_DESC];
        for (int i = 0; i < num_my_services; i++) {
            const char *svc = my_services[i];
            get_service_summary(svc, summary, sizeof(summary));
            char temp[MAX_LINE];
            strcpy(temp, summary);
            char *token = strtok(temp, "|");
            int j = 0;
            while (token && j < 5) {
                strncpy(parts[j], token, sizeof(parts[0]) - 1);
                parts[j][sizeof(parts[0]) - 1] = '\0';
                token = strtok(NULL, "|");
                j++;
            }
            if (j < 5) continue;

            status_color(parts[1], active_c, sizeof(active_c));
            enabled_color(parts[2], enabled_c, sizeof(enabled_c));

            if (strcmp(parts[0], "system") == 0) strcpy(scope_disp, "SYS");
            else if (strcmp(parts[0], "user") == 0) strcpy(scope_disp, "USR");
            else snprintf(scope_disp, sizeof(scope_disp), "%s???%s", ERR_COLOR, RESET_COLOR);

            printf("%-4d %-5s %-40s %-14s %-14s %-8s %s\n", i+1, scope_disp, svc, active_c, enabled_c, parts[4], parts[3]);
        }
    }

    printf("\n");
    printf("%sAktionen:%s\n", DIM_COLOR, RESET_COLOR);
    printf("  [Zahl]  Service-Detailseite (Favoriten)\n");
    printf("  a       Service zu Favoriten hinzufügen\n");
    printf("  r       Service aus Favoriten entfernen\n");
    printf("  R       Dashboard neu laden (Config)\n");
    printf("  B       ALLE Services (System + User) browsen\n");
    printf("  q       Beenden\n");
    printf("\n");
}

void add_service_interactive(const char *home) {
    print_header("Service zu Favoriten hinzufügen");
    printf("Gib den exakten Servicenamen ein (z.B. trainee_trainer-gunicorn.service):\n");
    printf("> ");
    char svc[MAX_LINE];
    if (fgets(svc, sizeof(svc), stdin) == NULL) return;
    svc[strcspn(svc, "\n")] = '\0';

    if (strlen(svc) == 0) {
        printf("%sAbgebrochen (kein Name eingegeben).%s\n", DIM_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    // Check if already exists
    for (int i = 0; i < num_my_services; i++) {
        if (strcmp(my_services[i], svc) == 0) {
            printf("%sService ist bereits in der Favoritenliste.%s\n", WARN_COLOR, RESET_COLOR);
            press_enter();
            return;
        }
    }

    char *scope = detect_scope(svc);
    if (strcmp(scope, "none") == 0) {
        printf("%sHinweis:%s Service wird aktuell weder als System- noch als User-Service gefunden.\n", WARN_COLOR, RESET_COLOR);
        printf("Trotzdem zu Favoriten hinzufügen? [y/N]\n");
        char answer[10];
        if (fgets(answer, sizeof(answer), stdin) == NULL) return;
        answer[strcspn(answer, "\n")] = '\0';
        if (tolower(answer[0]) != 'y') {
            printf("%sNicht hinzugefügt.%s\n", DIM_COLOR, RESET_COLOR);
            press_enter();
            return;
        }
    } else {
        printf("%sErkannt als %s-Service.%s\n", OK_COLOR, scope, RESET_COLOR);
    }

    if (num_my_services < MAX_SERVICES) {
        strcpy(my_services[num_my_services++], svc);
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
        printf("%sKeine Favoriten in der Liste.%s\n", DIM_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    for (int i = 0; i < num_my_services; i++) {
        printf("  %d) %s\n", i+1, my_services[i]);
    }

    printf("\n");
    printf("Nummer zum Entfernen (oder leer für Abbruch): ");
    char idx_str[MAX_LINE];
    if (fgets(idx_str, sizeof(idx_str), stdin) == NULL) return;
    idx_str[strcspn(idx_str, "\n")] = '\0';

    if (strlen(idx_str) == 0) {
        printf("%sAbgebrochen.%s\n", DIM_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    int idx = atoi(idx_str);
    if (idx < 1 || idx > num_my_services) {
        printf("%sUngültige Auswahl.%s\n", ERR_COLOR, RESET_COLOR);
        press_enter();
        return;
    }

    char removed[MAX_LINE];
    strcpy(removed, my_services[idx-1]);

    // Shift array
    for (int i = idx-1; i < num_my_services - 1; i++) {
        strcpy(my_services[i], my_services[i+1]);
    }
    num_my_services--;

    save_services(home);

    printf("%sService entfernt:%s %s\n", OK_COLOR, RESET_COLOR, removed);
    press_enter();
}

void build_all_services_list(const char *home) {
    num_all_services = 0;

    // System services from /etc/systemd/system/*.service
    DIR *dir = opendir("/etc/systemd/system");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && num_all_services < MAX_SERVICES) {
            if (entry->d_type == DT_REG && strstr(entry->d_name, ".service") != NULL) {
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
            if (entry->d_type == DT_REG && strstr(entry->d_name, ".service") != NULL) {
                // Check if already added (user services might overlap, but unlikely)
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

void render_all_services(void) {
    print_header("Alle Services System + User");

    if (num_all_services == 0) {
        printf("%sKeine Services gefunden.%s\n", ERR_COLOR, RESET_COLOR);
        printf("\n");
    } else {
        printf("%-4s %-5s %-40s %-14s %-14s %-8s %-s\n", "Nr.", "SCOPE", "SERVICE", "ACTIVE", "ENABLED", "PORT", "DESCRIPTION");
        printf("%-4s %-5s %-40s %-14s %-14s %-8s %-s\n", "----", "-----", "-------", "------", "-------", "----", "-----------");

        char summary[MAX_LINE];
        char active_c[64], enabled_c[64], scope_disp[16];
        char parts[5][MAX_DESC];
        for (int i = 0; i < num_all_services; i++) {
            const char *svc = all_services[i];
            get_service_summary(svc, summary, sizeof(summary));
            char temp[MAX_LINE];
            strcpy(temp, summary);
            char *token = strtok(temp, "|");
            int j = 0;
            while (token && j < 5) {
                strncpy(parts[j], token, sizeof(parts[0]) - 1);
                parts[j][sizeof(parts[0]) - 1] = '\0';
                token = strtok(NULL, "|");
                j++;
            }
            if (j < 5) continue;

            status_color(parts[1], active_c, sizeof(active_c));
            enabled_color(parts[2], enabled_c, sizeof(enabled_c));

            if (strcmp(parts[0], "system") == 0) strcpy(scope_disp, "SYS");
            else if (strcmp(parts[0], "user") == 0) strcpy(scope_disp, "USR");
            else snprintf(scope_disp, sizeof(scope_disp), "%s???%s", ERR_COLOR, RESET_COLOR);

            printf("%-4d %-5s %-40s %-14s %-14s %-8s %s\n", i+1, scope_disp, svc, active_c, enabled_c, parts[4], parts[3]);
        }
    }

    printf("\n");
    printf("%sAktionen:%s\n", DIM_COLOR, RESET_COLOR);
    printf("  [Zahl]  Service-Detailseite (aus kompletter Liste)\n");
    printf("  b       Zurück zum Dashboard\n");
    printf("\n");
}

void browse_all_services(void) {
    const char *home = getenv("HOME");
    build_all_services_list(home);

    while (1) {
        render_all_services();
        printf("Eingabe (Nummer oder b): ");
        char input[MAX_LINE];
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) continue;
        char first = tolower(input[0]);
        if (first == 'b') break;

        int idx = atoi(input);
        if (idx >= 1 && idx <= num_all_services) {
            service_detail_page(all_services[idx - 1]);
        } else {
            printf("%sUngültige Nummer.%s\n", ERR_COLOR, RESET_COLOR);
            press_enter();
        }
    }
}

void service_detail_page(const char *svc) {
    char scope_buf[16];
    strcpy(scope_buf, detect_scope(svc));

    while (1) {
        print_header("Service-Details: ");
        printf("%s\n", svc);

        char summary[MAX_LINE];
        get_service_summary(svc, summary, sizeof(summary));
        char parts[5][MAX_DESC];
        char temp[MAX_LINE];
        strcpy(temp, summary);
        char *token = strtok(temp, "|");
        int j = 0;
        while (token && j < 5) {
            strncpy(parts[j], token, sizeof(parts[0]) - 1);
            parts[j][sizeof(parts[0]) - 1] = '\0';
            token = strtok(NULL, "|");
            j++;
        }
        if (j < 5) break;

        char active_c[64], enabled_c[64];
        status_color(parts[1], active_c, sizeof(active_c));
        enabled_color(parts[2], enabled_c, sizeof(enabled_c));

        char scope_label[64];
        if (strcmp(parts[0], "system") == 0) strcpy(scope_label, "System-Service");
        else if (strcmp(parts[0], "user") == 0) strcpy(scope_label, "User-Service (~/.config/systemd/user)");
        else strcpy(scope_label, "NICHT GEFUNDEN");

        if (strcmp(parts[0], "none") == 0) {
            printf("%sACHTUNG:%s Service wird weder als System- noch als User-Service gefunden.\n", ERR_COLOR, RESET_COLOR);
            printf("\n");
        }

        printf("%sUebersicht%s\n", HEADER_COLOR, RESET_COLOR);
        printf("  Name:        %s\n", svc);
        printf("  Scope:       %s\n", scope_label);
        printf("  Active:      %s\n", active_c);
        printf("  Enabled:     %s\n", enabled_c);
        printf("  Port:        %s\n", parts[4]);
        printf("  Description: %s\n", parts[3]);

        // Additional info
        char act_ts[MAX_LINE] = {0}, inact_ts[MAX_LINE] = {0}, fragment_path[MAX_LINE] = {0};
        char unit_state[32] = {0}, substate[32] = {0};
        char cmd[MAX_LINE];
        const char *user_flag = (strcmp(parts[0], "system") == 0 ? "" : "--user");
        if (strcmp(parts[0], "none") != 0) {
            snprintf(cmd, sizeof(cmd), "systemctl %s show -p ActiveEnterTimestamp --value \"%s\" 2>/dev/null", user_flag, svc);
            execute_cmd(cmd, act_ts, sizeof(act_ts));
            snprintf(cmd, sizeof(cmd), "systemctl %s show -p InactiveEnterTimestamp --value \"%s\" 2>/dev/null", user_flag, svc);
            execute_cmd(cmd, inact_ts, sizeof(inact_ts));
            snprintf(cmd, sizeof(cmd), "systemctl %s show -p FragmentPath --value \"%s\" 2>/dev/null", user_flag, svc);
            execute_cmd(cmd, fragment_path, sizeof(fragment_path));
            snprintf(cmd, sizeof(cmd), "systemctl %s show -p LoadState --value \"%s\" 2>/dev/null", user_flag, svc);
            execute_cmd(cmd, unit_state, sizeof(unit_state));
            snprintf(cmd, sizeof(cmd), "systemctl %s show -p SubState --value \"%s\" 2>/dev/null", user_flag, svc);
            execute_cmd(cmd, substate, sizeof(substate));
        }

        printf("\n");
        printf("%sZusätzliche Infos%s\n", HEADER_COLOR, RESET_COLOR);
        printf("  LoadState:     %s\n", strlen(unit_state) ? unit_state : "unknown");
        printf("  SubState:      %s\n", strlen(substate) ? substate : "unknown");
        printf("  Fragment:      %s\n", strlen(fragment_path) ? fragment_path : "unknown");
        printf("  Active seit:   %s\n", strlen(act_ts) ? act_ts : "n/a");
        printf("  Inactive seit: %s\n", strlen(inact_ts) ? inact_ts : "n/a");

        printf("\n");
        printf("%sLetzte Logs (journalctl -u %s -n %d)%s\n", HEADER_COLOR, svc, DETAIL_LOG_LINES, RESET_COLOR);
        printf("\n");

        char log_cmd[MAX_LINE];
        int log_shown = 0;
        if (strcmp(parts[0], "system") == 0) {
            snprintf(log_cmd, sizeof(log_cmd), "journalctl -u \"%s\" -n %d --no-pager 2>/dev/null", svc, DETAIL_LOG_LINES);
            system(log_cmd);
            log_shown = 1;
        } else if (strcmp(parts[0], "user") == 0) {
            snprintf(log_cmd, sizeof(log_cmd), "journalctl --user -u \"%s\" -n %d --no-pager 2>/dev/null", svc, DETAIL_LOG_LINES);
            system(log_cmd);
            log_shown = 1;
        }
        if (!log_shown) {
            printf("%s(Keine Logs – Service nicht gefunden)%s\n", DIM_COLOR, RESET_COLOR);
        }

        printf("\n");
        printf("%sAktionen:%s\n", DIM_COLOR, RESET_COLOR);
        printf("  s   Start\n");
        printf("  t   Stop\n");
        printf("  r   Restart\n");
        printf("  e   Enable (beim Boot starten)\n");
        printf("  d   Disable\n");
        printf("  S   Vollständiges 'systemctl status' (mit less)\n");
        printf("  I   Vollständiges 'systemctl show' (mit less, alle Properties)\n");
        printf("  L   Live-Logs (journalctl -f, Abbruch mit Ctrl+C)\n");
        printf("  b   Zurück\n");
        printf("\n");
        printf("Aktion: ");
        char action[MAX_LINE];
        if (fgets(action, sizeof(action), stdin) == NULL) break;
        action[strcspn(action, "\n")] = '\0';
        if (strlen(action) == 0) continue;
        char act = tolower(action[0]);

        int handled = 0;
        const char *sudo_flag = (strcmp(parts[0], "system") == 0 ? "sudo " : "");
        const char *user_flag_act = (strcmp(parts[0], "system") == 0 ? "" : "--user ");
        if (act == 's' && strlen(action) == 1) {
            print_header("Start: ");
            printf("%s\n", svc);
            printf("Starte %s-Service (%s%s start %s)...\n", parts[0], sudo_flag, user_flag_act, svc);
            printf("\n");
            snprintf(log_cmd, sizeof(log_cmd), "%s%s start \"%s\"", sudo_flag, user_flag_act, svc);
            exec_simple(log_cmd);
            handled = 1;
        } else if (act == 't' && strlen(action) == 1) {
            print_header("Stop: ");
            printf("%s\n", svc);
            printf("Stoppe %s-Service (%s%s stop %s)...\n", parts[0], sudo_flag, user_flag_act, svc);
            printf("\n");
            snprintf(log_cmd, sizeof(log_cmd), "%s%s stop \"%s\"", sudo_flag, user_flag_act, svc);
            exec_simple(log_cmd);
            handled = 1;
        } else if (act == 'r' && strlen(action) == 1) {
            print_header("Restart: ");
            printf("%s\n", svc);
            printf("Neustart %s-Service (%s%s restart %s)...\n", parts[0], sudo_flag, user_flag_act, svc);
            printf("\n");
            snprintf(log_cmd, sizeof(log_cmd), "%s%s restart \"%s\"", sudo_flag, user_flag_act, svc);
            exec_simple(log_cmd);
            handled = 1;
        } else if (act == 'e' && strlen(action) == 1) {
            print_header("Enable: ");
            printf("%s\n", svc);
            printf("Enable %s-Service (%s%s enable %s)...\n", parts[0], sudo_flag, user_flag_act, svc);
            printf("\n");
            snprintf(log_cmd, sizeof(log_cmd), "%s%s enable \"%s\"", sudo_flag, user_flag_act, svc);
            exec_simple(log_cmd);
            handled = 1;
        } else if (act == 'd' && strlen(action) == 1) {
            print_header("Disable: ");
            printf("%s\n", svc);
            printf("Disable %s-Service (%s%s disable %s)...\n", parts[0], sudo_flag, user_flag_act, svc);
            printf("\n");
            snprintf(log_cmd, sizeof(log_cmd), "%s%s disable \"%s\"", sudo_flag, user_flag_act, svc);
            exec_simple(log_cmd);
            handled = 1;
        } else if (action[0] == 'S') {
            print_header("systemctl status: ");
            printf("%s\n", svc);
            printf("%s(Mit q aus 'less' zurück)%s\n", DIM_COLOR, RESET_COLOR);
            printf("\n");
            snprintf(log_cmd, sizeof(log_cmd), "systemctl %s status \"%s\" | less", user_flag_act, svc);
            system(log_cmd);
            handled = 1;
        } else if (action[0] == 'I') {
            print_header("systemctl show: ");
            printf("%s\n", svc);
            printf("%s(Mit q aus 'less' zurück – das kann VIEL sein)%s\n", DIM_COLOR, RESET_COLOR);
            printf("\n");
            snprintf(log_cmd, sizeof(log_cmd), "systemctl %s show \"%s\" | less", user_flag_act, svc);
            system(log_cmd);
            handled = 1;
        } else if (action[0] == 'L') {
            print_header("Live-Logs: ");
            printf("%s\n", svc);
            printf("%sBeende Live-View mit Ctrl+C%s\n", DIM_COLOR, RESET_COLOR);
            printf("\n");
            snprintf(log_cmd, sizeof(log_cmd), "journalctl %s -u \"%s\" -f", user_flag_act, svc);
            system(log_cmd);
            handled = 1;
        } else if (tolower(action[0]) == 'b') {
            break;
        }

        if (!handled) {
            printf("%sUnbekannte Aktion.%s\n", WARN_COLOR, RESET_COLOR);
        }
        if (handled && strlen(action) == 1) {
            press_enter();
        }
    }
}

void main_loop(const char *home) {
    while (1) {
        render_dashboard();
        printf("Eingabe: ");
        char input[MAX_LINE];
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) continue;
        char first = input[0];

        if (first == 'q' || first == 'Q') {
            printf("\n");
            printf("%sBye%s\n", DIM_COLOR, RESET_COLOR);
            exit(0);
        } else if (first == 'a' || first == 'A') {
            add_service_interactive(home);
        } else if (first == 'r' || first == 'R') {
            if (first == 'R') {
                load_services(home);
            } else {
                remove_service_interactive(home);
            }
        } else if (first == 'B') {
            browse_all_services();
        } else {
            int idx = atoi(input);
            if (idx >= 1 && idx <= num_my_services) {
                service_detail_page(my_services[idx - 1]);
            } else {
                printf("%sUngültige Nummer.%s\n", ERR_COLOR, RESET_COLOR);
                press_enter();
            }
        }
    }
}

int main(void) {
    check_systemctl();
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME not set\n");
        return 1;
    }
    load_services(home);
    main_loop(home);
    return 0;
}