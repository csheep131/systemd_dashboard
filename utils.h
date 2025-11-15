// utils.h – Sonstige Utilities

#ifndef UTILS_H
#define UTILS_H

int  get_resource_usage(long pid, float *cpu_pct, long *rss_kb);
void open_in_browser(const char *port_str);
void show_dependencies(const char *svc, const char *scope);

#endif
