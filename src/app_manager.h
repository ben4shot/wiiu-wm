#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include "config.h"
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

#define MAX_RUNNING_APPS 32

typedef struct {
    pid_t pid;
    char name[64];
    char exec[MAX_STR];
    char icon[MAX_STR];
    bool is_active;
    time_t launched_at;
    double launched_mono;   // reloj monotónico, para el filtro de doble toque
} RunningApp;

typedef struct {
    RunningApp running[MAX_RUNNING_APPS];
    int count;
    int active_index;
    char last_error[192];   // aviso para mostrar en pantalla (p. ej. programa no instalado)
    uint32_t error_seq;     // aumenta con cada aviso nuevo
} AppManager;

void app_manager_init(AppManager *mgr);
bool app_manager_launch(AppManager *mgr, const AppEntry *app);
bool app_manager_close(AppManager *mgr, int index);
void app_manager_focus(AppManager *mgr, int index);
void app_manager_next(AppManager *mgr);
void app_manager_prev(AppManager *mgr);
void app_manager_refresh(AppManager *mgr);

#endif // APP_MANAGER_H
