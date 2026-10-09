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
 * ==========================================================================*/
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int rc;
    if (strcmp(modo, "compress") == 0) {
        fprintf(stderr, "Comprimiendo '%s' -> '%s' con %d hilos...\n",
                src, dst, nthreads);
        rc = huf_compress(src, dst, nthreads);
    } else if (strcmp(modo, "decompress") == 0) {
        fprintf(stderr, "Descomprimiendo '%s' -> '%s' con %d hilos...\n",
                src, dst, nthreads);
        rc = huf_decompress(src, dst, nthreads);
    } else {
        usage(argv[0]);
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    if (rc == 0)
        fprintf(stderr, "Operacion completada en %.3f s.\n", secs);
    else
        fprintf(stderr, "La operacion fallo (codigo %d).\n", rc);

    return rc == 0 ? 0 : 2;
}
