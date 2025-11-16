#ifndef UI_H
#define UI_H

void init_ui(void);
void end_ui(void);
void show_message_ui(const char *msg);

void render_dashboard_ui(int selected_idx, int focus_on_list);
void browse_all_services_ui(const char *home);
void service_detail_page_ui(const char *svc);
void add_service_ui(const char *home);
void remove_service_ui(const char *home);

void get_input(char *buf, size_t bufsize);

void edit_unit_file(const char *svc, const char *scope);

#endif