// sys_dashboard.h – Zentrale Defines, Globals & Backend-Prototypen

#ifndef SYS_DASHBOARD_H
#define SYS_DASHBOARD_H

#include <stddef.h>

#define MAX_SERVICES 1000
#define MAX_LINE     1024
#define MAX_DESC     256

#define CONFIG_FILE "%s/.my_systemd_dashboard_services"
#define DEFAULT_SERVICES_COUNT 3
extern const char *DEFAULT_SERVICES[DEFAULT_SERVICES_COUNT];

#define DETAIL_LOG_LINES 20

// ANSI-Farbcodes (werden in status_color/enabled_color genutzt)
#define HEADER_COLOR "\033[36m"
#define OK_COLOR     "\033[32m"
#define WARN_COLOR   "\033[33m"
#define ERR_COLOR    "\033[31m"
#define DIM_COLOR    "\033[90m"
#define RESET_COLOR  "\033[0m"

// Globale Service-Listen
extern char my_services[MAX_SERVICES][MAX_LINE];
extern int  num_my_services;

extern char all_services[MAX_SERVICES][MAX_LINE];
extern int  num_all_services;

// sudo-Flag (leer bei root, sonst "sudo ")
extern const char *sudo_flag;

// Backend-Helfer
int  execute_cmd(const char *cmd, char *output, size_t max_output);
int  exec_simple(const char *cmd);

// Service-Config / -Infos
void load_services(const char *home);
void save_services(const char *home);
char *detect_scope(const char *svc);
char *guess_port(const char *svc, const char *scope);
void get_service_summary(const char *svc, char *summary, size_t bufsize);
void build_all_services_list(const char *home);

// Farb-Helfer (für Texte mit ANSI-Codes, auch in ncurses verwendbar)
void status_color(const char *s, char *buf, size_t bufsize);
void enabled_color(const char *s, char *buf, size_t bufsize);

// sudo-Init
void init_sudo_flag(void);

// systemctl-Check
void check_systemctl(void);

#endif
