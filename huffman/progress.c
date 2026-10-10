/* ============================================================================
 *  progress.c  -  Reporte de progreso en tiempo real (barra / porcentaje)
 * ----------------------------------------------------------------------------
 *  El estado de progreso es compartido entre los workers (que lo incrementan)
 *  y un lector (el hilo impresor de la CLI o el comando 'w' del editor).
 *
 *  Sincronizacion:
 *    - mutex 'mtx' protege los contadores -> sin condiciones de carrera.
 *    - cond 'cambio': cada avance hace broadcast; el lector DUERME en
 *      pthread_cond_wait hasta que haya algo nuevo que dibujar. No hay
 *      espera activa ni sondeo periodico (sin usleep).
 * ==========================================================================*/
#include "huffman.h"
#include <stdio.h>

void huf_progress_init(HufProgress *pg, uint32_t total) {
    pthread_mutex_init(&pg->mtx, NULL);
    pthread_cond_init(&pg->cambio, NULL);
    pg->total_bloques  = total;
    pg->bloques_hechos = 0;
    pg->fase           = HUF_FASE_INICIO;
    pg->terminado      = 0;
}

void huf_progress_destroy(HufProgress *pg) {
    pthread_cond_destroy(&pg->cambio);
    pthread_mutex_destroy(&pg->mtx);
}

void huf_progress_set_fase(HufProgress *pg, HufFase fase, uint32_t total) {
    pthread_mutex_lock(&pg->mtx);
    pg->fase           = fase;
    pg->total_bloques  = total;
    pg->bloques_hechos = 0;
    pthread_cond_broadcast(&pg->cambio);
    pthread_mutex_unlock(&pg->mtx);
}

void huf_progress_step(HufProgress *pg) {
    pthread_mutex_lock(&pg->mtx);
    pg->bloques_hechos++;
    pthread_cond_broadcast(&pg->cambio);
    pthread_mutex_unlock(&pg->mtx);
}

void huf_progress_finish(HufProgress *pg) {
    pthread_mutex_lock(&pg->mtx);
    pg->terminado = 1;
    pg->fase      = HUF_FASE_FIN;
    pthread_cond_broadcast(&pg->cambio);
    pthread_mutex_unlock(&pg->mtx);
}

void huf_progress_snapshot(HufProgress *pg, uint32_t *hechos, uint32_t *total,
                           HufFase *fase, int *terminado) {
    pthread_mutex_lock(&pg->mtx);
    if (hechos)    *hechos    = pg->bloques_hechos;
    if (total)     *total     = pg->total_bloques;
    if (fase)      *fase      = pg->fase;
    if (terminado) *terminado = pg->terminado;
    pthread_mutex_unlock(&pg->mtx);
}

uint32_t huf_progress_wait(HufProgress *pg, uint32_t visto, int *terminado) {
    pthread_mutex_lock(&pg->mtx);
    /* while (no if): protege contra despertares espurios. */
    while (pg->bloques_hechos == visto && !pg->terminado)
        pthread_cond_wait(&pg->cambio, &pg->mtx);
    uint32_t h = pg->bloques_hechos;
    if (terminado) *terminado = pg->terminado;
    pthread_mutex_unlock(&pg->mtx);
    return h;
}

static const char *nombre_fase(HufFase f) {
    switch (f) {
    case HUF_FASE_FRECUENCIAS: return "frecuencias";
    case HUF_FASE_BLOQUES:     return "bloques";
    case HUF_FASE_FIN:         return "listo";
    default:                   return "iniciando";
    }
}

/* Dibuja una barra tipo [#####-----]  52% (k/n bloques) */
void huf_progress_draw(FILE *out, uint32_t done, uint32_t total, HufFase fase) {
    const int width = 30;
    double ratio = (total == 0) ? 0.0 : (double)done / (double)total;
    if (ratio > 1.0) ratio = 1.0;
    int filled = (int)(ratio * width);

    fputs("\r[", out);
    for (int i = 0; i < width; i++)
        fputc(i < filled ? '#' : '-', out);
    fprintf(out, "] %3d%% (%u/%u %s)   ", (int)(ratio * 100.0), done, total,
            nombre_fase(fase));
    fflush(out);
}

static int pct_de(uint32_t h, uint32_t t) {
    return t == 0 ? 0 : (int)((uint64_t)h * 100 / t);
}

static void *printer_loop(void *arg) {
    HufProgress *pg = (HufProgress *)arg;
    int     pct_visto  = -1;           /* fuerza el primer dibujo */
    HufFase fase_vista = HUF_FASE_INICIO;
    for (;;) {
        uint32_t h, t; HufFase f; int fin;
        huf_progress_snapshot(pg, &h, &t, &f, &fin);
        if (fin) {
            huf_progress_draw(stderr, h, t, HUF_FASE_FIN);
            fputc('\n', stderr);
            break;
        }
        /* Solo se redibuja si cambia el porcentaje o la fase. */
        if (pct_de(h, t) != pct_visto || f != fase_vista) {
            huf_progress_draw(stderr, h, t, f);
            pct_visto = pct_de(h, t); fase_vista = f;
        }
        /* Duerme hasta el proximo avance: sin espera activa. */
        pthread_mutex_lock(&pg->mtx);
        while (pg->bloques_hechos == h && pg->fase == f && !pg->terminado)
            pthread_cond_wait(&pg->cambio, &pg->mtx);
        pthread_mutex_unlock(&pg->mtx);
    }
    return NULL;
}

int huf_progress_spawn_printer(HufProgress *pg, pthread_t *tid) {
    return pthread_create(tid, NULL, printer_loop, pg) == 0 ? 0 : -1;
}
