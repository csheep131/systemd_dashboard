#ifndef UTILS_H
#define UTILS_H

void open_in_browser_ui(const char *port_str);
int get_resource_usage(long pid, float *cpu_pct, long *rss_kb);
void show_dependencies_ui(const char *svc, const char *scope);
void edit_unit_file_ui(const char *svc, const char *scope);
void press_enter(void);

#endif
