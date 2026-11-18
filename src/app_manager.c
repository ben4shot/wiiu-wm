#include "app_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

void app_manager_init(AppManager *mgr) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->count = 0;
    mgr->active_index = -1;
}

void app_manager_refresh(AppManager *mgr) {
    // Limpia procesos finalizados sin bloquear
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->running[i].pid > 0) {
            int status;
            pid_t res = waitpid(mgr->running[i].pid, &status, WNOHANG);
            if (res > 0 || (res == -1 && kill(mgr->running[i].pid, 0) == -1)) {
                printf("[AppManager] Proceso '%s' (PID %d) finalizó.\n", mgr->running[i].name, mgr->running[i].pid);
                if (res > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                    // sh -c devuelve 127 cuando no encuentra la orden
                    snprintf(mgr->last_error, sizeof(mgr->last_error), "'%s' no está instalado: %.120s",
                             mgr->running[i].name, mgr->running[i].exec);
                    mgr->error_seq++;
                } else if (res > 0 && WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
                           time(NULL) - mgr->running[i].launched_at < 3) {
                    snprintf(mgr->last_error, sizeof(mgr->last_error), "'%s' terminó con error %d al abrirse",
                             mgr->running[i].name, WEXITSTATUS(status));
                    mgr->error_seq++;
                }
                // Desplazar lista hacia la izquierda
                for (int j = i; j < mgr->count - 1; j++) {
                    mgr->running[j] = mgr->running[j + 1];
                }
                mgr->count--;
                if (mgr->active_index >= mgr->count) {
                    mgr->active_index = mgr->count - 1;
                }
                i--;
            }
        }
    }
}

bool app_manager_launch(AppManager *mgr, const AppEntry *app) {
    if (!app || !app->exec[0]) return false;

    // Solo se filtra el doble toque accidental (< 0,8 s); abrir varias instancias está permitido.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    for (int i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->running[i].exec, app->exec) == 0 && now - mgr->running[i].launched_mono < 0.8) {
            printf("[AppManager] '%s' se está abriendo; se ignora el toque repetido.\n", app->name);
            return false;
        }
    }

    if (mgr->count >= MAX_RUNNING_APPS) {
        fprintf(stderr, "[AppManager] Límite de aplicaciones alcanzado.\n");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[AppManager] fork falló");
        return false;
    }

    if (pid == 0) {
        // Proceso hijo: ejecutar mediante shell para soportar argumentos y tuberías.
        // Que no herede las señales ignoradas/bloqueadas del shell.
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        setsid();
        execl("/bin/sh", "sh", "-c", app->exec, (char *)NULL);
        _exit(127);
    }

    // Proceso padre
    RunningApp *r = &mgr->running[mgr->count];
    memset(r, 0, sizeof(*r));
    r->pid = pid;
    snprintf(r->name, sizeof(r->name), "%s", app->name);
    snprintf(r->exec, sizeof(r->exec), "%s", app->exec);
    snprintf(r->icon, sizeof(r->icon), "%s", app->icon);
    r->is_active = true;
    r->launched_at = time(NULL);
    r->launched_mono = now;

    mgr->active_index = mgr->count;
    mgr->count++;
    printf("[AppManager] Lanzada '%s' con PID %d (Total activas: %d)\n", app->name, pid, mgr->count);
    return true;
}

bool app_manager_close(AppManager *mgr, int index) {
    if (index < 0 || index >= mgr->count) return false;

    pid_t pid = mgr->running[index].pid;
    printf("[AppManager] Cerrando aplicación '%s' (PID %d)\n", mgr->running[index].name, pid);

    if (pid > 0) {
        kill(pid, SIGTERM);
    }

    // Remover de la lista
    for (int i = index; i < mgr->count - 1; i++) {
        mgr->running[i] = mgr->running[i + 1];
    }
    mgr->count--;

    if (mgr->active_index >= mgr->count) {
        mgr->active_index = mgr->count - 1;
    }
    return true;
}

void app_manager_focus(AppManager *mgr, int index) {
    if (index < 0 || index >= mgr->count) return;

    for (int i = 0; i < mgr->count; i++) {
        mgr->running[i].is_active = (i == index);
    }
    mgr->active_index = index;
    printf("[AppManager] Enfocada app '%s' (PID %d)\n", mgr->running[index].name, mgr->running[index].pid);
}

void app_manager_next(AppManager *mgr) {
    if (mgr->count <= 1) return;
    int next = (mgr->active_index + 1) % mgr->count;
    app_manager_focus(mgr, next);
}

void app_manager_prev(AppManager *mgr) {
    if (mgr->count <= 1) return;
    int prev = (mgr->active_index - 1 + mgr->count) % mgr->count;
    app_manager_focus(mgr, prev);
}
