# systemd_dashboard
man kann damit systemd services schön anzeigen und verwalten.
erspart systemctl/journalctl tiperei
install:
gcc sys_dashboard.c ui.c utils.c -lncurses -o sysdash
