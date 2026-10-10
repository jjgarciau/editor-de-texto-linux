/* ============================================================================
 *  main.c  -  CLI del Compresor Huffman Concurrente
 * ----------------------------------------------------------------------------
 *  Uso:
 *    ./huffman compress   <entrada>  <salida.huf>  [n_hilos]
 *    ./huffman decompress <entrada.huf> <salida>   [n_hilos]
 *
 *  Este binario es INDEPENDIENTE del editor. Para la integracion "en segundo
 *  plano" sin congelar la UI, el editor (o cualquier shell) puede invocarlo
 *  como proceso aparte, por ejemplo:
 *
 *      ./huffman compress nota.txt nota.huf 4 &
 *
 *  El '&' lo deja en background; la barra de progreso se imprime por stderr.
 *  Ademas, el editor del Parcial 1 integra el compresor como HILO de fondo
 *  (comandos z/u/j/w/k de 'edi', ver shell/editor_huf.c).
 *  Ctrl+C cancela de forma limpia (ver instalar_senales).
 * ==========================================================================*/
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/*  Manejo de senales: Ctrl+C (SIGINT) o SIGTERM NO matan el proceso   */
/*  a medias. El manejador solo levanta una bandera (operacion segura  */
/*  dentro de un manejador: async-signal-safe); los workers la revisan */
/*  antes de cada bloque, el coordinador termina, se hace join de todos */
/*  los hilos, se libera la memoria y se borra la salida parcial.      */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_cancel = 0;

static void on_signal(int sig) {
    (void)sig;
    g_cancel = 1;
}

static void instalar_senales(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;          /* read/write se reanudan solos */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int nproc_default(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    if (n > 16) n = 16;
    return (int)n;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Compresor Huffman Concurrente (Parcial 2 - SO)\n"
        "Uso:\n"
        "  %s compress   <entrada>      <salida.huf> [n_hilos]\n"
        "  %s decompress <entrada.huf>  <salida>     [n_hilos]\n"
        "\n"
        "Si no se indica n_hilos, se usa el numero de nucleos disponibles.\n",
        prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 4) { usage(argv[0]); return 1; }

    const char *modo = argv[1];
    const char *src  = argv[2];
    const char *dst  = argv[3];
    int nthreads = (argc >= 5) ? atoi(argv[4]) : nproc_default();
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    instalar_senales();

    /* Progreso compartido + hilo impresor (duerme en una cond var). */
    HufProgress pg;
    huf_progress_init(&pg, 0);
    pthread_t printer;
    int hay_printer = (huf_progress_spawn_printer(&pg, &printer) == 0);

    HufOpts op;
    memset(&op, 0, sizeof(op));
    op.pg     = &pg;
    op.cancel = &g_cancel;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int rc;
    if (strcmp(modo, "compress") == 0) {
        fprintf(stderr, "Comprimiendo '%s' -> '%s' con %d hilos...\n",
                src, dst, nthreads);
        rc = huf_compress_ex(src, dst, nthreads, &op);
    } else if (strcmp(modo, "decompress") == 0) {
        fprintf(stderr, "Descomprimiendo '%s' -> '%s' con %d hilos...\n",
                src, dst, nthreads);
        rc = huf_decompress_ex(src, dst, nthreads, &op);
    } else {
        huf_progress_finish(&pg);
        if (hay_printer) pthread_join(printer, NULL);
        huf_progress_destroy(&pg);
        usage(argv[0]);
        return 1;
    }

    huf_progress_finish(&pg);              /* por si la operacion no llego */
    if (hay_printer) pthread_join(printer, NULL);   /* join limpio      */
    huf_progress_destroy(&pg);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    if (rc == 0)
        fprintf(stderr, "Operacion completada en %.3f s.\n", secs);
    else if (rc == 1)
        fprintf(stderr, "Operacion CANCELADA por senal; salida parcial eliminada.\n");
    else
        fprintf(stderr, "La operacion fallo (codigo %d).\n", rc);

    if (rc == 1) return 130;               /* convencion: cancelado por senal */
    return rc == 0 ? 0 : 2;
}
