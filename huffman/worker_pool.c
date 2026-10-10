/* ============================================================================
 *  worker_pool.c  -  Pool de hilos trabajadores + cola de tareas sincronizada
 * ----------------------------------------------------------------------------
 *  Mecanismos de sincronizacion (rubrica, 25 pts):
 *    - 1 mutex protege la cola de tareas.
 *    - cond 'not_empty': los workers DUERMEN si no hay tareas (NO busy-waiting).
 *    - cond 'all_done' : pool_wait() duerme hasta que la cola este vacia y no
 *                        haya tareas en vuelo.
 *  No hay espera activa en ningun punto: siempre pthread_cond_wait.
 * ==========================================================================*/
#include "huffman.h"
#include <stdlib.h>
#include <stdio.h>

typedef struct Task {
    huf_task_fn   fn;
    void         *arg;
    struct Task  *next;
} Task;


struct WorkerPool {
    pthread_t      *threads;
    int             nthreads;

    Task           *head;
    Task           *tail;
    int             pending;     /* tareas en cola + en ejecucion         */

    int             shutdown;    /* 1 -> los hilos deben terminar         */

    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;   /* hay trabajo o shutdown                */
    pthread_cond_t  all_done;    /* pending llego a 0                     */
};

/* ------------------------------------------------------------------ */
/*  Bucle de cada hilo trabajador                                     */
/* ------------------------------------------------------------------ */
static void *worker_loop(void *arg) {
    WorkerPool *p = (WorkerPool *)arg;

    for (;;) {
        pthread_mutex_lock(&p->mtx);

        /* Espera pasiva mientras no haya tareas y no sea shutdown. */
        while (p->head == NULL && !p->shutdown)
            pthread_cond_wait(&p->not_empty, &p->mtx);

        if (p->shutdown && p->head == NULL) {
            pthread_mutex_unlock(&p->mtx);
            break;
        }

        /* Desencolar una tarea. */
        Task *t = p->head;
        p->head = t->next;
        if (p->head == NULL) p->tail = NULL;

        pthread_mutex_unlock(&p->mtx);

        /* Ejecutar FUERA del lock para permitir paralelismo real. */
        t->fn(t->arg);
        free(t);

        /* Marcar la tarea como completada. */
        pthread_mutex_lock(&p->mtx);
        p->pending--;
        if (p->pending == 0)
            pthread_cond_signal(&p->all_done);
        pthread_mutex_unlock(&p->mtx);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
WorkerPool *pool_create(int nthreads) {
    if (nthreads < 1) nthreads = 1;
    WorkerPool *p = calloc(1, sizeof(WorkerPool));
    if (!p) return NULL;

    p->threads  = malloc(sizeof(pthread_t) * nthreads);
    if (!p->threads) { free(p); return NULL; }
    p->nthreads = nthreads;
    p->head = p->tail = NULL;
    p->pending  = 0;
    p->shutdown = 0;

    pthread_mutex_init(&p->mtx, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    pthread_cond_init(&p->all_done, NULL);

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[i], NULL, worker_loop, p) != 0) {
            /* Si falla, apagamos lo creado hasta aqui. */
            p->nthreads = i;
            pool_destroy(p);
            return NULL;
        }
    }
    return p;
}

/* ------------------------------------------------------------------ */
int pool_submit(WorkerPool *p, huf_task_fn fn, void *arg) {
    Task *t = malloc(sizeof(Task));
    if (!t) return -1;
    t->fn   = fn;
    t->arg  = arg;
    t->next = NULL;

    pthread_mutex_lock(&p->mtx);
    if (p->tail) p->tail->next = t;
    else         p->head = t;
    p->tail = t;
    p->pending++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->mtx);
    return 0;
}

/* ------------------------------------------------------------------ */
void pool_wait(WorkerPool *p) {
    pthread_mutex_lock(&p->mtx);
    while (p->pending > 0)
        pthread_cond_wait(&p->all_done, &p->mtx);
    pthread_mutex_unlock(&p->mtx);
}

/* ------------------------------------------------------------------ */
void pool_destroy(WorkerPool *p) {
    if (!p) return;

    pthread_mutex_lock(&p->mtx);
    p->shutdown = 1;
    pthread_cond_broadcast(&p->not_empty);   /* despierta a todos       */
    pthread_mutex_unlock(&p->mtx);

    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);   /* join limpio             */

    /* Liberar tareas que pudieran quedar sin ejecutar. */
    Task *t = p->head;
    while (t) { Task *n = t->next; free(t); t = n; }

    pthread_mutex_destroy(&p->mtx);
    pthread_cond_destroy(&p->not_empty);
    pthread_cond_destroy(&p->all_done);
    free(p->threads);
    free(p);
}
