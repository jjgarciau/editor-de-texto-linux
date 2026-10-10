/* ============================================================================
 *  freq_parallel.c  -  Fase de Conteo de Frecuencias CONCURRENTE
 * ----------------------------------------------------------------------------
 *  Varios hilos leen regiones distintas del archivo EN PARALELO (cada uno con
 *  su propio descriptor y su propio offset) y acumulan frecuencias en un
 *  arreglo LOCAL: durante el conteo no comparten nada -> cero contencion.
 *
 *  Al terminar su region, CADA HILO suma su parcial al arreglo GLOBAL dentro
 *  de una region critica protegida por un mutex. Asi se combinan las dos
 *  tecnicas que pide el enunciado: reduccion local + sincronizacion adecuada.
 *  El mutex se toma una sola vez por hilo (256 sumas), no una vez por byte.
 * ==========================================================================*/
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* Estado compartido por todos los hilos del conteo. */
typedef struct {
    uint64_t              *global;   /* arreglo global de frecuencias      */
    pthread_mutex_t        mtx;      /* protege 'global'                   */
    volatile sig_atomic_t *cancel;   /* abortar si != 0 (puede ser NULL)   */
} FreqShared;

typedef struct {
    const char *path;
    off_t       start;              /* offset de inicio en el archivo    */
    off_t       end;                /* offset final (exclusivo)          */
    uint64_t    local[HUF_NSYM];    /* reduccion local de este hilo      */
    FreqShared *sh;
    int         ok;
} FreqJob;

static void *freq_worker(void *arg) {
    FreqJob *job = (FreqJob *)arg;
    job->ok = 0;
    memset(job->local, 0, sizeof(job->local));

    int fd = open(job->path, O_RDONLY);
    if (fd < 0) return NULL;

    if (lseek(fd, job->start, SEEK_SET) == (off_t)-1) { close(fd); return NULL; }

    uint8_t *buf = malloc(HUF_CHUNK_SIZE);
    if (!buf) { close(fd); return NULL; }
    off_t remaining = job->end - job->start;

    while (remaining > 0) {
        if (HUF_CANCELADO(job->sh->cancel)) {      /* cancelacion */
            free(buf); close(fd); return NULL;
        }
        size_t  want = remaining < (off_t)HUF_CHUNK_SIZE ? (size_t)remaining
                                                        : HUF_CHUNK_SIZE;
        ssize_t got  = huf_read_full(fd, buf, want);
        if (got < 0) { free(buf); close(fd); return NULL; }
        if (got == 0) break;                 /* EOF: el archivo se encogio */
        for (ssize_t i = 0; i < got; i++)
            job->local[buf[i]]++;            /* sin lock: dato privado */
        remaining -= got;
    }
    free(buf);
    close(fd);

    /* ---- Region critica minima: combinar el parcial en el global ---- */
    pthread_mutex_lock(&job->sh->mtx);
    for (int s = 0; s < HUF_NSYM; s++)
        job->sh->global[s] += job->local[s];
    pthread_mutex_unlock(&job->sh->mtx);

    job->ok = 1;
    return NULL;
}

int huf_count_frequencies_ex(const char *path, int nthreads,
                             uint64_t freq[HUF_NSYM], uint64_t *filesize,
                             volatile sig_atomic_t *cancel) {
    memset(freq, 0, sizeof(uint64_t) * HUF_NSYM);
    *filesize = 0;

    struct stat st;
    if (stat(path, &st) != 0) {
        perror("stat");
        return -1;
    }
    off_t total = st.st_size;
    *filesize = (uint64_t)total;

    if (total == 0) return 0;             /* archivo vacio: 0 frecuencias */
    if (nthreads < 1) nthreads = 1;
    if ((off_t)nthreads > total) nthreads = (int)total;

    FreqJob   *jobs    = calloc(nthreads, sizeof(FreqJob));
    pthread_t *threads = calloc(nthreads, sizeof(pthread_t));
    if (!jobs || !threads) { free(jobs); free(threads); return -1; }

    FreqShared sh;
    sh.global = freq;
    sh.cancel = cancel;
    pthread_mutex_init(&sh.mtx, NULL);

    off_t per = total / nthreads;
    int creados = 0, rc = 0;
    for (int i = 0; i < nthreads; i++) {
        jobs[i].path  = path;
        jobs[i].start = per * i;
        jobs[i].end   = (i == nthreads - 1) ? total : per * (i + 1);
        jobs[i].sh    = &sh;
        if (pthread_create(&threads[i], NULL, freq_worker, &jobs[i]) != 0) {
            rc = -1;                       /* no se pudo crear: abortar  */
            break;
        }
        creados++;
    }

    /* join de TODOS los hilos creados, haya o no error. */
    for (int i = 0; i < creados; i++) {
        pthread_join(threads[i], NULL);
        if (!jobs[i].ok) rc = -1;
    }

    pthread_mutex_destroy(&sh.mtx);
    free(jobs);
    free(threads);
    return rc;
}

int huf_count_frequencies(const char *path, int nthreads,
                          uint64_t freq[HUF_NSYM], uint64_t *filesize) {
    return huf_count_frequencies_ex(path, nthreads, freq, filesize, NULL);
}
