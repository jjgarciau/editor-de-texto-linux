/**
 * ====================================================================================
 *  editor_huf.c  -  INTEGRACION DEL COMPRESOR HUFFMAN CONCURRENTE CON EL EDITOR
 *                   (Parcial 2 - Concurrencia y Sincronizacion)
 * ====================================================================================
 *  Comandos nuevos del editor:
 *    z [salida.huf] [hilos]        comprime el archivo abierto EN SEGUNDO PLANO
 *    u <entrada.huf> <salida> [h]  descomprime EN SEGUNDO PLANO
 *    j                             estado/progreso de la tarea (porcentaje)
 *    w                             monitor en vivo de la barra hasta que termine
 *    k                             cancela la tarea en curso
 *
 *  1) EJECUCION EN SEGUNDO PLANO (background worker)
 *     'z'/'u' crean UN hilo de fondo (pthread_create) que llama a
 *     huf_compress_ex()/huf_decompress_ex(). Ese hilo es el COORDINADOR y a su
 *     vez usa el pool de hilos trabajadores. El hilo del REPL vuelve de
 *     inmediato al prompt: la interfaz nunca se congela.
 *
 *  2) PROGRESO EN TIEMPO REAL
 *     Los workers actualizan un HufProgress (mutex + cond). El prompt muestra
 *     el porcentaje ("edi:nota.txt [z 45%]>"), 'j' dibuja la barra y 'w' la
 *     redibuja en vivo durmiendo en la variable de condicion (sin sondeo).
 *
 *  3) CONDICIONES DE CARRERA CON LA EDICION  ->  pthread_rwlock (lectores/escritores)
 *     Mientras el compresor LEE el archivo fuente mantiene un bloqueo de LECTURA
 *     (rdlock). Los comandos que MODIFICAN el archivo (a, i, d, x) piden el
 *     bloqueo de ESCRITURA con pthread_rwlock_trywrlock(): si esta ocupado, el
 *     comando se rechaza con un aviso en lugar de esperar (no se congela la UI).
 *     Los comandos de solo lectura (p, s, m, y) pueden convivir con el lector.
 *     En cuanto el compresor termina de leer la fuente (callback 'fuente_leida')
 *     suelta el rdlock: la edicion vuelve a estar disponible mientras los
 *     bloques todavia se codifican y escriben (region critica minima).
 *
 *     Para cerrar la ventana entre pthread_create() y el rdlock, el REPL espera
 *     (cond var 'listo') a que el hilo confirme que ya tomo el bloqueo antes de
 *     devolver el prompt: ningun comando de edicion puede colarse antes.
 *
 *     El archivo de SALIDA tampoco puede editarse mientras se escribe, y si es
 *     el archivo abierto se reindexa al terminar.
 *
 *  4) RECURSOS
 *     El hilo de fondo se une con pthread_join al terminar (al siguiente prompt),
 *     y al salir del editor ('q' o Ctrl+D) se espera a que termine: nunca queda un
 *     hilo huerfano ni un archivo a medias.
 * ====================================================================================
 */

#include "shell.h"
#include "editor.h"
#include "../huffman/huffman.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

typedef enum { JOB_LIBRE = 0, JOB_CORRIENDO, JOB_TERMINADO } JobEstado;

typedef struct {
    pthread_t             hilo;
    char                  modo;              /* 'z' comprimir, 'u' descomprimir */
    char                  src[PATH_MAX];
    char                  dst[PATH_MAX];
    int                   nhilos;

    HufProgress           pg;                /* progreso compartido con workers */
    volatile sig_atomic_t cancel;            /* 'k' lo pone en 1                */

    pthread_mutex_t       mtx;               /* protege los campos de abajo     */
    pthread_cond_t        listo;             /* handshake: rdlock ya tomado     */
    int                   bloqueo_tomado;
    int                   fuente_liberada;
    JobEstado             estado;
    int                   rc;
    double                segundos;
} HufJob;

static HufJob g_job = {
    .mtx    = PTHREAD_MUTEX_INITIALIZER,
    .listo  = PTHREAD_COND_INITIALIZER,
    .estado = JOB_LIBRE,
};

/* Bloqueo lectores/escritores sobre el archivo fuente de la tarea. */
static pthread_rwlock_t g_archivo = PTHREAD_RWLOCK_INITIALIZER;

/* ------------------------------------------------------------------------------------
 *  Utilidades
 * ------------------------------------------------------------------------------------ */
static int hilos_por_defecto(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 2;
    if (n > 16) n = 16;
    return (int)n;
}

/* 1 si 'ruta' y el archivo abierto en el editor son el mismo inodo. */
static int mismo_archivo(Editor *ed, const char *ruta)
{
    struct stat a, b;
    if (!ed->abierto || ed->fd < 0) return 0;
    if (fstat(ed->fd, &a) != 0) return 0;
    if (stat(ruta, &b) != 0) return 0;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

static JobEstado estado_job(void)
{
    pthread_mutex_lock(&g_job.mtx);
    JobEstado e = g_job.estado;
    pthread_mutex_unlock(&g_job.mtx);
    return e;
}

/* Callback invocado POR EL HILO DE FONDO cuando ya leyo toda la fuente.
   Suelta el rdlock (solo el hilo que lo tomo puede soltarlo). */
static void liberar_fuente(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_job.mtx);
    int ya = g_job.fuente_liberada;
    g_job.fuente_liberada = 1;
    pthread_mutex_unlock(&g_job.mtx);
    if (!ya) pthread_rwlock_unlock(&g_archivo);
}

/* ------------------------------------------------------------------------------------
 *  Cuerpo del hilo de fondo (background worker)
 * ------------------------------------------------------------------------------------ */
static void *hilo_tarea(void *arg)
{
    (void)arg;
    struct timespec t0, t1;

    /* 1. Tomar el bloqueo de LECTURA sobre la fuente y avisar al REPL. */
    pthread_rwlock_rdlock(&g_archivo);
    pthread_mutex_lock(&g_job.mtx);
    g_job.bloqueo_tomado = 1;
    pthread_cond_signal(&g_job.listo);
    pthread_mutex_unlock(&g_job.mtx);

    /* 2. Ejecutar la operacion concurrente. */
    HufOpts op;
    memset(&op, 0, sizeof(op));
    op.pg           = &g_job.pg;
    op.cancel       = &g_job.cancel;
    op.fuente_leida = liberar_fuente;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = (g_job.modo == 'z')
           ? huf_compress_ex(g_job.src, g_job.dst, g_job.nhilos, &op)
           : huf_decompress_ex(g_job.src, g_job.dst, g_job.nhilos, &op);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    liberar_fuente(NULL);                   /* por si una ruta de error no aviso */

    /* 3. Publicar el resultado; el REPL hara el join. */
    pthread_mutex_lock(&g_job.mtx);
    g_job.rc       = rc;
    g_job.segundos = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    g_job.estado   = JOB_TERMINADO;
    pthread_mutex_unlock(&g_job.mtx);
    return NULL;
}

/* ------------------------------------------------------------------------------------
 *  Lanzar una tarea
 * ------------------------------------------------------------------------------------ */
static int lanzar(Editor *ed, char modo, const char *src, const char *dst, int nhilos)
{
    if (estado_job() != JOB_LIBRE) {
        fprintf(stderr, COLOR_ERROR "Ya hay una tarea en segundo plano. Usa 'j' para ver su "
                "estado o 'k' para cancelarla.\n" COLOR_RESET);
        return -1;
    }
    if (strlen(src) >= PATH_MAX || strlen(dst) >= PATH_MAX) {
        fprintf(stderr, COLOR_ERROR "Ruta demasiado larga.\n" COLOR_RESET);
        return -1;
    }
    struct stat s1, s2;
    if (stat(src, &s1) != 0) { ed_fallo(src); return -1; }
    if (stat(dst, &s2) == 0 && s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino) {
        fprintf(stderr, COLOR_ERROR "La entrada y la salida no pueden ser el mismo archivo.\n" COLOR_RESET);
        return -1;
    }
    if (mismo_archivo(ed, dst)) {
        fprintf(stderr, COLOR_ERROR "La salida no puede ser el archivo abierto en el editor "
                "(se destruiria su contenido).\n" COLOR_RESET);
        return -1;
    }

    g_job.modo = modo;
    strcpy(g_job.src, src);
    strcpy(g_job.dst, dst);
    g_job.nhilos          = nhilos > 0 ? nhilos : hilos_por_defecto();
    g_job.cancel          = 0;
    g_job.bloqueo_tomado  = 0;
    g_job.fuente_liberada = 0;
    g_job.rc              = 0;
    huf_progress_init(&g_job.pg, 0);

    pthread_mutex_lock(&g_job.mtx);
    g_job.estado = JOB_CORRIENDO;
    pthread_mutex_unlock(&g_job.mtx);

    if (pthread_create(&g_job.hilo, NULL, hilo_tarea, NULL) != 0) {
        ed_fallo("pthread_create");
        huf_progress_destroy(&g_job.pg);
        pthread_mutex_lock(&g_job.mtx);
        g_job.estado = JOB_LIBRE;
        pthread_mutex_unlock(&g_job.mtx);
        return -1;
    }

    /* Handshake: no devolver el prompt hasta que el hilo tenga el rdlock. */
    pthread_mutex_lock(&g_job.mtx);
    while (!g_job.bloqueo_tomado)
        pthread_cond_wait(&g_job.listo, &g_job.mtx);
    pthread_mutex_unlock(&g_job.mtx);

    printf(COLOR_RESULT "%s en segundo plano: %s -> %s (%d hilos).\n" COLOR_RESET,
           modo == 'z' ? "Comprimiendo" : "Descomprimiendo", src, dst, g_job.nhilos);
    printf(COLOR_INFO "Puedes seguir usando el editor. 'j' estado, 'w' ver en vivo, "
           "'k' cancelar.\n" COLOR_RESET);
    return 0;
}

/* z [salida.huf] [hilos] */
int ed_huf_comprimir(Editor *ed, const char *args)
{
    char salida[PATH_MAX] = "";
    int  nh = 0;

    if (!ed->abierto) {
        fprintf(stderr, COLOR_ERROR "No hay archivo abierto. Usa: o <archivo>\n" COLOR_RESET);
        return -1;
    }
    if (args && *args) {
        if (sscanf(args, "%4095s %d", salida, &nh) < 1) salida[0] = '\0';
    }
    if (salida[0] == '\0') {
        if (snprintf(salida, sizeof(salida), "%s.huf", ed->ruta) >= (int)sizeof(salida)) {
            fprintf(stderr, COLOR_ERROR "Ruta demasiado larga.\n" COLOR_RESET);
            return -1;
        }
    }
    return lanzar(ed, 'z', ed->ruta, salida, nh);
}

/* u <entrada.huf> <salida> [hilos] */
int ed_huf_descomprimir(Editor *ed, const char *args)
{
    char ent[PATH_MAX], sal[PATH_MAX];
    int  nh = 0;
    if (!args || sscanf(args, "%4095s %4095s %d", ent, sal, &nh) < 2) {
        fprintf(stderr, COLOR_ERROR "Uso: u <entrada.huf> <salida> [hilos]\n" COLOR_RESET);
        return -1;
    }
    return lanzar(ed, 'u', ent, sal, nh);
}

/* ------------------------------------------------------------------------------------
 *  Exclusion mutua con los comandos que modifican el archivo
 * ------------------------------------------------------------------------------------ */
/* Devuelve 1 si el comando de escritura puede seguir (y deja tomado el wrlock
   cuando corresponde: hay que llamar a ed_huf_fin_escritura). 0 si se rechaza. */
static int g_wr_tomado = 0;

int ed_huf_permitir_escritura(Editor *ed)
{
    g_wr_tomado = 0;
    if (estado_job() != JOB_CORRIENDO) return 1;

    
    /* El archivo de salida se esta escribiendo: no se toca hasta que termine. */
    if (mismo_archivo(ed, g_job.dst)) {
        fprintf(stderr, COLOR_ERROR "Este archivo es la SALIDA de la tarea en segundo plano; "
                "espera a que termine ('j').\n" COLOR_RESET);
        return 0;
    }
    if (!mismo_archivo(ed, g_job.src)) return 1;   /* otra tarea, otro archivo */

    /* Lectores/escritores: si el compresor aun lee la fuente, EBUSY. */
    int r = pthread_rwlock_trywrlock(&g_archivo);
    if (r == EBUSY) {
        uint32_t h, t; HufFase f;
        huf_progress_snapshot(&g_job.pg, &h, &t, &f, NULL);
        fprintf(stderr, COLOR_ERROR "Archivo en uso: el compresor lo esta leyendo (%s). "
                "Edicion rechazada para evitar una condicion de carrera; intenta en un momento "
                "o cancela con 'k'.\n" COLOR_RESET,
                f == HUF_FASE_FRECUENCIAS ? "contando frecuencias" : "cargando bloques");
        return 0;
    }
    if (r != 0) { errno = r; ed_fallo("pthread_rwlock_trywrlock"); return 0; }
    g_wr_tomado = 1;
    return 1;
}

void ed_huf_fin_escritura(void)
{
    if (g_wr_tomado) {
        pthread_rwlock_unlock(&g_archivo);
        g_wr_tomado = 0;
    }
}

/* ------------------------------------------------------------------------------------
 *  Estado, monitor y cancelacion
 * ------------------------------------------------------------------------------------ */
static int porcentaje(uint32_t h, uint32_t t, HufFase f)
{
    if (t == 0 || f == HUF_FASE_FRECUENCIAS || f == HUF_FASE_INICIO) return 0;
    return (int)((uint64_t)h * 100 / t);
}

/* Texto corto para el prompt, p. ej. " [z 45%]". Cadena vacia si no hay tarea. */
void ed_huf_etiqueta(char *buf, size_t n)
{
    buf[0] = '\0';
    if (estado_job() != JOB_CORRIENDO) return;
    uint32_t h, t; HufFase f;
    huf_progress_snapshot(&g_job.pg, &h, &t, &f, NULL);
    snprintf(buf, n, " [%c %d%%]", g_job.modo, porcentaje(h, t, f));
}

void ed_huf_estado(void)
{
    JobEstado e = estado_job();
    if (e == JOB_LIBRE) {
        printf(COLOR_INFO "No hay tareas en segundo plano.\n" COLOR_RESET);
        return;
    }
    uint32_t h, t; HufFase f;
    huf_progress_snapshot(&g_job.pg, &h, &t, &f, NULL);
    printf(COLOR_TITLE "%s %s -> %s (%d hilos)\n" COLOR_RESET,
           g_job.modo == 'z' ? "Compresion" : "Descompresion",
           g_job.src, g_job.dst, g_job.nhilos);
    huf_progress_draw(stdout, h, t, f);
    printf("\n");
}

void ed_huf_monitor(void)
{
    if (estado_job() == JOB_LIBRE) {
        printf(COLOR_INFO "No hay tareas en segundo plano.\n" COLOR_RESET);
        return;
    }
    uint32_t h, t; HufFase f; int fin = 0;
    huf_progress_snapshot(&g_job.pg, &h, &t, &f, &fin);
    huf_progress_draw(stdout, h, t, f);
    while (!fin) {
        /* Duerme en la cond var del progreso: se despierta en cada bloque. */
        uint32_t nuevo = huf_progress_wait(&g_job.pg, h, &fin);
        huf_progress_snapshot(&g_job.pg, &h, &t, &f, &fin);
        (void)nuevo;
        huf_progress_draw(stdout, h, t, f);
    }
    printf("\n");
}

void ed_huf_cancelar(void)
{
    if (estado_job() != JOB_CORRIENDO) {
        printf(COLOR_INFO "No hay tareas en curso.\n" COLOR_RESET);
        return;
    }
    __atomic_store_n(&g_job.cancel, 1, __ATOMIC_RELAXED);  /* los workers lo ven antes de cada bloque */
    printf(COLOR_INFO "Cancelacion solicitada.\n" COLOR_RESET);
}

/* join + informe del resultado. Bloquea solo si el hilo aun no termino. */
static void finalizar(Editor *ed)
{
    pthread_join(g_job.hilo, NULL);           /* join limpio del hilo de fondo */

    if (g_job.rc == 0) {
        struct stat a, b;
        long long ta = (stat(g_job.src, &a) == 0) ? (long long)a.st_size : -1;
        long long tb = (stat(g_job.dst, &b) == 0) ? (long long)b.st_size : -1;
        printf(COLOR_RESULT "[huffman] %s terminada en %.3f s: %s (%lld B) -> %s (%lld B)\n"
               COLOR_RESET, g_job.modo == 'z' ? "Compresion" : "Descompresion",
               g_job.segundos, g_job.src, ta, g_job.dst, tb);
        /* Si la salida es el archivo abierto, su indice quedo obsoleto. */
        if (mismo_archivo(ed, g_job.dst)) ed_indexar(ed);
    } else if (g_job.rc == 1) {
        printf(COLOR_INFO "[huffman] Tarea cancelada; se elimino la salida parcial.\n" COLOR_RESET);
    } else {
        fprintf(stderr, COLOR_ERROR "[huffman] La tarea fallo; no se dejo salida parcial.\n"
                COLOR_RESET);
    }

    huf_progress_destroy(&g_job.pg);
    pthread_mutex_lock(&g_job.mtx);
    g_job.estado = JOB_LIBRE;
    pthread_mutex_unlock(&g_job.mtx);
}

/* Se llama antes de cada prompt: si la tarea termino, join + informe. */
void ed_huf_revisar(Editor *ed)
{
    if (estado_job() == JOB_TERMINADO) finalizar(ed);
}

/* Al salir del editor: esperar a la tarea (no se pierde trabajo ni quedan hilos). */
void ed_huf_apagar(Editor *ed)
{
    if (estado_job() == JOB_LIBRE) return;
    if (estado_job() == JOB_CORRIENDO) {
        printf(COLOR_INFO "Esperando a que termine la tarea en segundo plano...\n" COLOR_RESET);
        ed_huf_monitor();
    }
    finalizar(ed);
}
