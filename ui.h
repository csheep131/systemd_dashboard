// ui.h – ncurses UI-Schnittstelle

#ifndef UI_H
#define UI_H

#include <stddef.h>

void init_ui(void);
void end_ui(void);

void render_dashboard_ui(void);
void get_input(char *buf, size_t buflen);

void show_message_ui(const char *msg);

void browse_all_services_ui(const char *home);
void service_detail_page_ui(const char *svc);

// Favoriten bearbeiten (ncurses)
void add_service_ui(const char *home);
void remove_service_ui(const char *home);

#endif
