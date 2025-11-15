#ifndef UTILS_H
#define UTILS_H

void open_in_browser(const char *port_str);

int get_resource_usage(long pid, float *cpu_pct, long *rss_kb);


void show_dependencies(const char *svc, const char *scope);

#endif
