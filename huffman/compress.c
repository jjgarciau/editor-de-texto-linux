/* ============================================================================
 *  compress.c  -  Compresion CONCURRENTE por bloques + ensamblado ordenado
 * ----------------------------------------------------------------------------
 *  Flujo:
 *    1) Conteo de frecuencias concurrente (freq_parallel.c).
 *    2) Lectura del archivo en bloques de HUF_CHUNK_SIZE (instantanea del
 *       contenido). Se verifica que coincida con lo contado: si el archivo
 *       cambio entre ambas lecturas se aborta en vez de producir basura.
 *       -> Aqui se avisa 'fuente_leida': el editor ya puede volver a escribir.
 *    3) Construccion del arbol de Huffman global (huffman.c).  [secuencial]
 *    4) Cada bloque se codifica EN PARALELO por el pool de hilos.
 *    5) El hilo COORDINADOR (el que llamo a huf_compress) escribe los bloques
 *       en el ORDEN secuencial correcto, durmiendo en una variable de
 *       condicion mientras el siguiente bloque no este listo.
 *
 *  Sincronizacion:
 *    - El coordinador espera el bloque i con while(!done[i]) cond_wait.
 *    - Cada worker, al terminar el bloque i (bien, con error o cancelado),
 *      marca done[i]=1 y hace broadcast. SIEMPRE marca done: si no, el
 *      coordinador quedaria dormido para siempre (interbloqueo).
 *    - No hay espera activa en ningun punto.
 * ==========================================================================*/
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  Estado de un bloque comprimido                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t  index;        /* orden del bloque                        */
    uint8_t  *in;           /* datos crudos del bloque                 */
    size_t    in_len;
    uint8_t  *out;          /* datos comprimidos (bits empaquetados)   */
    size_t    out_bytes;
    uint64_t  out_bits;     /* bits validos escritos                   */
    int       done;         /* 1 cuando el worker termino este bloque  */
    int       error;        /* 1 si el bloque fallo o se cancelo       */
} Block;

/* ------------------------------------------------------------------ */
/*  Contexto compartido entre workers y coordinador                   */
/* ------------------------------------------------------------------ */
typedef struct {
    Block                 *blocks;
    uint32_t               nblocks;
    const HufCode         *codes;   /* tabla de codigos (solo lectura)  */
    pthread_mutex_t        mtx;
    pthread_cond_t         cond;    /* se avisa cuando un bloque termina */
    HufProgress           *pg;
    volatile sig_atomic_t *cancel;
} CompressCtx;

/* Argumento por tarea de worker. */
typedef struct {
    CompressCtx *ctx;
    uint32_t     block_index;
} CompressTaskArg;

/* ------------------------------------------------------------------ */
/*  Codificar un bloque: convierte bytes crudos en secuencia de bits  */
/* ------------------------------------------------------------------ */
static void compress_block_task(void *arg) {
    CompressTaskArg *ta  = (CompressTaskArg *)arg;
    CompressCtx     *ctx = ta->ctx;
    Block           *blk = &ctx->blocks[ta->block_index];
    const HufCode   *codes = ctx->codes;
    int              err = 0;

    if (HUF_CANCELADO(ctx->cancel)) {
        err = 1;                                   /* cancelado: no trabajar */
    } else {
        /* 1a pasada: tamanio exacto de la salida (suma de longitudes). */
        uint64_t nbits = 0;
        for (size_t i = 0; i < blk->in_len; i++)
            nbits += codes[blk->in[i]].length;

        uint8_t *out = calloc(1, (size_t)((nbits + 7) / 8) + 1);
        if (!out) {
            err = 1;                               /* sin memoria */
        } else {
            /* 2a pasada: empaquetar los bits de cada simbolo. */
            uint64_t bitpos = 0;
            for (size_t i = 0; i < blk->in_len; i++) {
                const HufCode *c = &codes[blk->in[i]];
                for (uint16_t b = 0; b < c->length; b++) {
                    int bit = (c->bits[b / 8] >> (7 - (b % 8))) & 1;
                    if (bit)
                        out[bitpos / 8] |= (uint8_t)(1u << (7 - (bitpos % 8)));
                    bitpos++;
                }
            }
            blk->out       = out;
            blk->out_bits  = bitpos;
            blk->out_bytes = (size_t)((bitpos + 7) / 8);
        }
    }

    /* Marcar bloque listo (SIEMPRE) y avisar al coordinador. */
    pthread_mutex_lock(&ctx->mtx);
    blk->error = err;
    blk->done  = 1;
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->mtx);

    if (ctx->pg) huf_progress_step(ctx->pg);
    free(ta);
}

/* ------------------------------------------------------------------ */
/*  Escribir cabecera: MAGIC, tam original, tabla de frecuencias      */
/* ------------------------------------------------------------------ */
static int write_header(int fd, uint64_t orig_size,
                        const uint64_t freq[HUF_NSYM], uint32_t nblocks) {
    uint8_t magic[4] = { HUF_MAGIC0, HUF_MAGIC1, HUF_MAGIC2, HUF_MAGIC3 };
    if (huf_write_all(fd, magic, 4) != 0) return -1;
    if (huf_write_all(fd, &orig_size, sizeof(orig_size)) != 0) return -1;

    uint32_t nsym = 0;
    for (int s = 0; s < HUF_NSYM; s++) if (freq[s] > 0) nsym++;
    if (huf_write_all(fd, &nsym, sizeof(nsym)) != 0) return -1;

    for (int s = 0; s < HUF_NSYM; s++) {
        if (freq[s] == 0) continue;
        uint8_t  sym = (uint8_t)s;
        uint64_t f   = freq[s];
        if (huf_write_all(fd, &sym, 1) != 0) return -1;
        if (huf_write_all(fd, &f, sizeof(f)) != 0) return -1;
    }

    if (huf_write_all(fd, &nblocks, sizeof(nblocks)) != 0) return -1;
    return 0;
}

/* Libera los bloques leidos (in/out) y el arreglo. */
static void free_blocks(Block *blocks, uint32_t n) {
    if (!blocks) return;
    for (uint32_t i = 0; i < n; i++) {
        free(blocks[i].in);
        free(blocks[i].out);
    }
    free(blocks);
}

/* ------------------------------------------------------------------ */
/*  Leer el archivo completo en bloques y verificar que no cambio.    */
/* ------------------------------------------------------------------ */
static int read_blocks(const char *src, uint64_t filesize, uint32_t nblocks,
                       const uint64_t freq[HUF_NSYM], Block **out) {
    *out = NULL;
    int infd = open(src, O_RDONLY);
    if (infd < 0) { perror("open src"); return -1; }

    Block *blocks = calloc(nblocks ? nblocks : 1, sizeof(Block));
    if (!blocks) { close(infd); return -1; }

    uint64_t check[HUF_NSYM];
    memset(check, 0, sizeof(check));
    uint64_t total = 0;

    for (uint32_t i = 0; i < nblocks; i++) {
        Block *b = &blocks[i];
        b->index = i;
        b->in    = malloc(HUF_CHUNK_SIZE);
        if (!b->in) { close(infd); free_blocks(blocks, nblocks); return -1; }
        ssize_t got = huf_read_full(infd, b->in, HUF_CHUNK_SIZE);
        if (got < 0) { perror("read"); close(infd); free_blocks(blocks, nblocks); return -1; }
        b->in_len = (size_t)got;
        total += (uint64_t)got;
        for (ssize_t k = 0; k < got; k++) check[b->in[k]]++;
    }
    /* Un byte extra al final significa que el archivo crecio. */
    uint8_t extra;
    ssize_t more = huf_read_full(infd, &extra, 1);
    close(infd);

    if (total != filesize || more != 0 ||
        memcmp(check, freq, sizeof(check)) != 0) {
        fprintf(stderr, "El archivo cambio durante la compresion; se aborta.\n");
        free_blocks(blocks, nblocks);
        return -1;
    }
    *out = blocks;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  huf_compress_ex: orquesta todo el proceso                         */
/* ------------------------------------------------------------------ */
int huf_compress_ex(const char *src, const char *dst, int nthreads,
                    const HufOpts *op) {
    HufProgress           *pg     = op ? op->pg : NULL;
    volatile sig_atomic_t *cancel = op ? op->cancel : NULL;
    int fuente_avisada = 0;
    int rc = 0;

#define AVISAR_FUENTE() do { if (!fuente_avisada && op && op->fuente_leida) { \
        op->fuente_leida(op->fuente_arg); } fuente_avisada = 1; } while (0)

    uint64_t freq[HUF_NSYM];
    uint64_t filesize = 0;

    if (pg) huf_progress_set_fase(pg, HUF_FASE_FRECUENCIAS, 0);
    if (huf_count_frequencies_ex(src, nthreads, freq, &filesize, cancel) != 0) {
        AVISAR_FUENTE();
        if (HUF_CANCELADO(cancel)) { if (pg) huf_progress_finish(pg); return 1; }
        fprintf(stderr, "Error en el conteo de frecuencias.\n");
        if (pg) huf_progress_finish(pg);
        return -1;
    }

    uint32_t nblocks = (filesize == 0)
                     ? 0
                     : (uint32_t)((filesize + HUF_CHUNK_SIZE - 1) / HUF_CHUNK_SIZE);

    /* Instantanea del contenido + verificacion de consistencia. */
    Block *blocks = NULL;
    if (read_blocks(src, filesize, nblocks, freq, &blocks) != 0) {
        AVISAR_FUENTE();
        if (pg) huf_progress_finish(pg);
        return -1;
    }
    AVISAR_FUENTE();          /* ya no se vuelve a leer la fuente */

    HufNode *tree = huf_build_tree(freq);
    if (nblocks > 0 && !tree) {
        free_blocks(blocks, nblocks);
        if (pg) huf_progress_finish(pg);
        return -1;
    }
    HufCode  codes[HUF_NSYM];
    huf_build_codes(tree, codes);

    int outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
        perror("open dst");
        free_blocks(blocks, nblocks); huf_free_tree(tree);
        if (pg) huf_progress_finish(pg);
        return -1;
    }

    if (write_header(outfd, filesize, freq, nblocks) != 0) {
        perror("write cabecera");
        rc = -1;
        goto fin;
    }
    if (nblocks == 0) goto fin;          /* archivo vacio: solo cabecera */

    CompressCtx ctx;
    ctx.blocks  = blocks;
    ctx.nblocks = nblocks;
    ctx.codes   = codes;
    ctx.pg      = pg;
    ctx.cancel  = cancel;
    pthread_mutex_init(&ctx.mtx, NULL);
    pthread_cond_init(&ctx.cond, NULL);
    if (pg) huf_progress_set_fase(pg, HUF_FASE_BLOQUES, nblocks);

    WorkerPool *pool = pool_create(nthreads);
    if (!pool) {
        fprintf(stderr, "No se pudo crear el pool de hilos.\n");
        rc = -1;
    } else {
        /* Encolar la codificacion de cada bloque (productor). */
        uint32_t encolados = 0;
        for (uint32_t i = 0; i < nblocks; i++) {
            CompressTaskArg *ta = malloc(sizeof(CompressTaskArg));
            if (!ta) break;
            ta->ctx = &ctx;
            ta->block_index = i;
            if (pool_submit(pool, compress_block_task, ta) != 0) { free(ta); break; }
            encolados++;
        }
        if (encolados < nblocks) rc = -1;

        
        /* Coordinador: escribe en ORDEN, esperando cada bloque con cond. */
        for (uint32_t i = 0; i < encolados && rc == 0; i++) {
            pthread_mutex_lock(&ctx.mtx);
            while (!ctx.blocks[i].done)
                pthread_cond_wait(&ctx.cond, &ctx.mtx);
            int err = ctx.blocks[i].error;
            pthread_mutex_unlock(&ctx.mtx);

            if (err) { rc = HUF_CANCELADO(cancel) ? 1 : -1; break; }

            Block   *b  = &ctx.blocks[i];
            uint64_t ob = b->out_bytes;
            if (huf_write_all(outfd, &b->out_bits, sizeof(b->out_bits)) != 0 ||
                huf_write_all(outfd, &ob, sizeof(ob)) != 0 ||
                (ob > 0 && huf_write_all(outfd, b->out, b->out_bytes) != 0)) {
                perror("write bloque");
                rc = -1;
            }
            free(b->out); b->out = NULL;   /* liberar en cuanto se escribe */
        }

        /* Esperar a TODAS las tareas encoladas y unir los hilos. */
        pool_wait(pool);
        pool_destroy(pool);
    }
    if (rc == 0 && HUF_CANCELADO(cancel)) rc = 1;

    pthread_mutex_destroy(&ctx.mtx);
    pthread_cond_destroy(&ctx.cond);

fin:
    free_blocks(blocks, nblocks);
    if (close(outfd) != 0 && rc == 0) { perror("close dst"); rc = -1; }
    huf_free_tree(tree);
    if (rc != 0) unlink(dst);            /* no dejar un .huf a medias */
    if (pg) huf_progress_finish(pg);
    return rc;
#undef AVISAR_FUENTE
}

int huf_compress(const char *src, const char *dst, int nthreads) {
    return huf_compress_ex(src, dst, nthreads, NULL);
}
