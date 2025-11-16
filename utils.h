#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>

// ANSI-Farben (für CLI-Ausgaben)
#define HEADER_COLOR   "\033[1;36m"
#define OK_COLOR       "\033[1;32m"
#define WARN_COLOR     "\033[1;33m"
#define ERR_COLOR      "\033[1;31m"
#define DIM_COLOR      "\033[0;90m"
#define RESET_COLOR    "\033[0m"

// Funktionsdeklarationen
void open_in_browser(const char *port_str);
int get_resource_usage(long pid, float *cpu_pct, long *rss_kb);
void show_dependencies(const char *svc, const char *scope);
void press_enter(void);

#endif // UTILS_H