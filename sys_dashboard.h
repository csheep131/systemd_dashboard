#ifndef SYS_DASHBOARD_H
#define SYS_DASHBOARD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Defines
#define MAX_SERVICES        1000
#define MAX_LINE            1024
#define MAX_DESC            256
#define DEFAULT_SERVICES_COUNT 3
#define CONFIG_FILE         "%s/.config/sys-dashboard/services.txt"

// Globale Variablen (extern in anderen Dateien)
extern char my_services[MAX_SERVICES][MAX_LINE];
extern int  num_my_services;
extern char all_services[MAX_SERVICES][MAX_LINE];
extern int  num_all_services;
extern const char *sudo_flag;

// Funktionen aus sys_dashboard.c
void init_sudo_flag(void);
int execute_cmd(const char *cmd, char *output, size_t max_output);
int exec_simple(const char *cmd);
void check_systemctl(void);
void load_services(const char *home);
void save_services(const char *home);
void get_service_summary(const char *svc, char *summary, size_t bufsize);
char *detect_scope(const char *svc);
char *guess_port(const char *svc, const char *scope);
void build_all_services_list(const char *home);
void add_service_interactive(const char *home);
void remove_service_interactive(const char *home);
void main_loop(const char *home);

// UI-Funktionen (jetzt in ui.h deklariert, hier nur für Kompatibilität)

#endif // SYS_DASHBOARD_H