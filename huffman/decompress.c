/* ============================================================================
 *  decompress.c  -  Descompresion CONCURRENTE + ensamblado ordenado
 * ----------------------------------------------------------------------------
 *  1) Lee y VALIDA la cabecera; reconstruye EL MISMO arbol de Huffman a partir
 *     de la tabla de frecuencias (garantiza salida identica al original).
 *  2) Lee todos los bloques comprimidos (bits + tamanios).
 *     -> Aqui se avisa 'fuente_leida'.
 *  3) Decodifica cada bloque EN PARALELO (pool de hilos).
 *  4) El coordinador escribe los bloques decodificados en ORDEN secuencial.
 *
 *  Sincronizacion identica a compress.c: mutex + cond por "bloque listo",
 *  sin busy-waiting; los workers SIEMPRE marcan done (incluso con error).
 * ==========================================================================*/
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
    uint32_t  index;
    uint8_t  *in;          /* bits comprimidos                         */
    size_t    in_bytes;
    uint64_t  in_bits;     /* bits validos a decodificar               */
    uint8_t  *out;         /* bytes decodificados                      */
    size_t    out_len;     /* cuantos bytes produce este bloque        */
    int       done;
    int       error;
} DBlock;

typedef struct {
    DBlock                *blocks;
    uint32_t               nblocks;
    const HufNode         *tree;    /* arbol para decodificar (solo lectura) */
    pthread_mutex_t        mtx;
    pthread_cond_t         cond;
    HufProgress           *pg;
    volatile sig_atomic_t *cancel;
} DecompCtx;

typedef struct {
    DecompCtx *ctx;
    uint32_t   block_index;
} DecompTaskArg;

/* ------------------------------------------------------------------ */
/*  Decodificar un bloque recorriendo el arbol bit a bit              */
/* ------------------------------------------------------------------ */
static void decompress_block_task(void *arg) {
    DecompTaskArg *ta  = (DecompTaskArg *)arg;
    DecompCtx     *ctx = ta->ctx;
    DBlock        *blk = &ctx->blocks[ta->block_index];
    const HufNode *root = ctx->tree;
    int            err = 0;

    uint8_t *out = NULL;
    size_t   produced = 0;

    if (HUF_CANCELADO(ctx->cancel)) {
        err = 1;
    } else if ((out = malloc(blk->out_len ? blk->out_len : 1)) == NULL) {
        err = 1;
    } else {
        const HufNode *node = root;
        /* Arbol de un solo simbolo: cada bit (siempre 0) es un simbolo. */
        int un_simbolo = (root->left && root->left->symbol >= 0 && root->right == NULL);

        for (uint64_t bitpos = 0; bitpos < blk->in_bits && produced < blk->out_len; bitpos++) {
            if (un_simbolo) {
                out[produced++] = (uint8_t)root->left->symbol;
                continue;
            }
            int bit = (blk->in[bitpos / 8] >> (7 - (bitpos % 8))) & 1;
            node = bit ? node->right : node->left;
            if (!node) { err = 1; break; }          /* datos corruptos */
            if (node->symbol >= 0) {
                out[produced++] = (uint8_t)node->symbol;
                node = root;
            }
        }
        if (produced != blk->out_len) err = 1;      /* bloque incompleto */
    }

    blk->out = out;

    pthread_mutex_lock(&ctx->mtx);
    blk->error = err;
    blk->done  = 1;
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->mtx);

    if (ctx->pg) huf_progress_step(ctx->pg);
    free(ta);
}

static int read_exact(int fd, void *buf, size_t n) {
    ssize_t got = huf_read_full(fd, buf, n);
    return (got == (ssize_t)n) ? 0 : -1;
}

static void free_dblocks(DBlock *b, uint32_t n) {
    if (!b) return;
    for (uint32_t i = 0; i < n; i++) { free(b[i].in); free(b[i].out); }
    free(b);
}

/* ------------------------------------------------------------------ */
int huf_decompress_ex(const char *src, const char *dst, int nthreads,
                      const HufOpts *op) {
    HufProgress           *pg     = op ? op->pg : NULL;
    volatile sig_atomic_t *cancel = op ? op->cancel : NULL;
    int  rc = 0;
    int  avisada = 0;
    HufNode *tree = NULL;
    DBlock  *blocks = NULL;
    uint32_t nblocks = 0;
    int infd = -1, outfd = -1;

#define AVISAR_FUENTE() do { if (!avisada && op && op->fuente_leida) { \
        op->fuente_leida(op->fuente_arg); } avisada = 1; } while (0)
#define FALLA(msg) do { fprintf(stderr, "%s\n", msg); rc = -1; goto fin; } while (0)

    if (pg) huf_progress_set_fase(pg, HUF_FASE_FRECUENCIAS, 0);

    infd = open(src, O_RDONLY);
    if (infd < 0) { perror("open src"); rc = -1; goto fin; }

    /* --- Cabecera --- */
    uint8_t magic[4];
    if (read_exact(infd, magic, 4) != 0 ||
        magic[0] != HUF_MAGIC0 || magic[1] != HUF_MAGIC1 ||
        magic[2] != HUF_MAGIC2 || magic[3] != HUF_MAGIC3)
        FALLA("Formato invalido: no es un archivo .huf valido.");

    uint64_t orig_size = 0;
    uint32_t nsym = 0;
    if (read_exact(infd, &orig_size, sizeof(orig_size)) != 0 ||
        read_exact(infd, &nsym, sizeof(nsym)) != 0 || nsym > HUF_NSYM)
        FALLA("Cabecera .huf corrupta.");

    uint64_t freq[HUF_NSYM];
    memset(freq, 0, sizeof(freq));
    for (uint32_t i = 0; i < nsym; i++) {
        uint8_t  sym; uint64_t f;
        if (read_exact(infd, &sym, 1) != 0 ||
            read_exact(infd, &f, sizeof(f)) != 0)
            FALLA("Tabla de frecuencias corrupta.");
        freq[sym] = f;
    }

    if (read_exact(infd, &nblocks, sizeof(nblocks)) != 0)
        FALLA("Cabecera .huf corrupta.");
    uint64_t esperados = (orig_size + HUF_CHUNK_SIZE - 1) / HUF_CHUNK_SIZE;
    if ((uint64_t)nblocks != esperados)
        FALLA("Numero de bloques inconsistente con el tamanio original.");

    /* --- Reconstruir arbol identico --- */
    tree = huf_build_tree(freq);
    if (nblocks > 0 && !tree) FALLA("No se pudo reconstruir el arbol.");

    /* --- Leer bloques comprimidos --- */
    blocks = calloc(nblocks ? nblocks : 1, sizeof(DBlock));
    if (!blocks) FALLA("Sin memoria.");

    uint64_t remaining_orig = orig_size;
    for (uint32_t i = 0; i < nblocks; i++) {
        DBlock *b = &blocks[i];
        b->index = i;
        uint64_t bits = 0, bytes = 0;
        if (read_exact(infd, &bits, sizeof(bits)) != 0 ||
            read_exact(infd, &bytes, sizeof(bytes)) != 0 ||
            bytes != (bits + 7) / 8 ||
            bytes > (uint64_t)HUF_CHUNK_SIZE * (HUF_MAX_CODE_LEN / 8))
            FALLA("Cabecera de bloque corrupta.");
        b->in_bits  = bits;
        b->in_bytes = (size_t)bytes;
        b->in = malloc(bytes ? bytes : 1);
        if (!b->in) FALLA("Sin memoria.");
        if (bytes > 0 && read_exact(infd, b->in, bytes) != 0)
            FALLA("Archivo .huf truncado.");
        /* Cuantos bytes decodifica este bloque (ultimo puede ser menor). */
        b->out_len = (remaining_orig >= HUF_CHUNK_SIZE)
                   ? HUF_CHUNK_SIZE : (size_t)remaining_orig;
        remaining_orig -= b->out_len;
    }
    close(infd); infd = -1;
    AVISAR_FUENTE();

    outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) { perror("open dst"); rc = -1; goto fin; }
    if (nblocks == 0) goto fin;              /* archivo original vacio */

    DecompCtx ctx;
    ctx.blocks  = blocks;
    ctx.nblocks = nblocks;
    ctx.tree    = tree;
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
        uint32_t encolados = 0;
        for (uint32_t i = 0; i < nblocks; i++) {
            DecompTaskArg *ta = malloc(sizeof(DecompTaskArg));
            if (!ta) break;
            ta->ctx = &ctx;
            ta->block_index = i;
            if (pool_submit(pool, decompress_block_task, ta) != 0) { free(ta); break; }
            encolados++;
        }
        if (encolados < nblocks) rc = -1;

        /* --- Coordinador: escribe en orden --- */
        for (uint32_t i = 0; i < encolados && rc == 0; i++) {
            pthread_mutex_lock(&ctx.mtx);
            while (!ctx.blocks[i].done)
                pthread_cond_wait(&ctx.cond, &ctx.mtx);
            int err = ctx.blocks[i].error;
            pthread_mutex_unlock(&ctx.mtx);

            if (err) {
                if (HUF_CANCELADO(cancel)) rc = 1;
                else { fprintf(stderr, "Bloque %u corrupto.\n", i); rc = -1; }
                break;
            }
            DBlock *b = &ctx.blocks[i];
            if (b->out_len > 0 && huf_write_all(outfd, b->out, b->out_len) != 0) {
                perror("write");
                rc = -1;
            }
            free(b->out); b->out = NULL;
        }

        pool_wait(pool);
        pool_destroy(pool);
    }
    if (rc == 0 && HUF_CANCELADO(cancel)) rc = 1;
    pthread_mutex_destroy(&ctx.mtx);
    pthread_cond_destroy(&ctx.cond);

fin:
    AVISAR_FUENTE();
    if (infd >= 0) close(infd);
    if (outfd >= 0) {
        if (close(outfd) != 0 && rc == 0) { perror("close dst"); rc = -1; }
        if (rc != 0) unlink(dst);           /* no dejar salida a medias */
    }
    free_dblocks(blocks, nblocks);
    huf_free_tree(tree);
    if (pg) huf_progress_finish(pg);
    return rc;
#undef FALLA
#undef AVISAR_FUENTE
}

int huf_decompress(const char *src, const char *dst, int nthreads) {
    return huf_decompress_ex(src, dst, nthreads, NULL);
}
