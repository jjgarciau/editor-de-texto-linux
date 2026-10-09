/* ============================================================================
 *  huffman.h  -  Compresor/Descompresor de archivos Huffman CONCURRENTE
 *  Parcial 2 - Sistemas Operativos: Concurrencia y Sincronizacion
 *  Alternativa 1: Compresor de Archivos Huffman Concurrente Integrado al Editor
 * ----------------------------------------------------------------------------
 *  Este header expone la API publica del modulo. El modulo es TOTALMENTE
 *  independiente del editor (carpeta shell/): se compila como binario propio
 *  'huffman' y puede invocarse desde la CLI del editor como proceso en 2do
 *  plano, sin modificar ni una linea del codigo del editor.
 *
 *  Formato del archivo comprimido (.huf):
 *    [MAGIC 4B]['H''U''F''1']
 *    [uint64  tam_original]        tamanio del archivo original en bytes
 *    [uint32  num_simbolos]        cuantas entradas de tabla de frecuencias
 *    [ (uint8 simbolo, uint64 frecuencia) * num_simbolos ]   -> tabla
 *    [uint32  num_bloques]
 *    [ por cada bloque, en ORDEN: (uint64 bits_validos, uint64 bytes, datos) ]
 *
 *  La tabla de frecuencias se guarda para poder reconstruir el MISMO arbol de
 *  Huffman en la descompresion (corteccion algoritmica -> salida identica).
 * ==========================================================================*/
#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define HUF_MAGIC0 'H'
#define HUF_MAGIC1 'U'
#define HUF_MAGIC2 'F'
#define HUF_MAGIC3 '1'

#define HUF_NSYM        256          /* numero de simbolos posibles (bytes)   */
#define HUF_CHUNK_SIZE  (64 * 1024)  /* tamanio de cada bloque: 64 KB         */
#define HUF_MAX_CODE_LEN 256         /* longitud maxima de un codigo (bits)   */

/* ------------------------------------------------------------------ */
/*  Nodo del arbol de Huffman                                         */
/* ------------------------------------------------------------------ */
typedef struct HufNode {
    uint64_t         freq;    /* frecuencia acumulada                 */
    int32_t          symbol;  /* 0..255 si es hoja; -1 si es interno  */
    struct HufNode  *left;
    struct HufNode  *right;
} HufNode;

/* ------------------------------------------------------------------ */
/*  Codigo canonico por simbolo (secuencia de bits)                   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  bits[HUF_MAX_CODE_LEN / 8]; /* bits empaquetados          */
    uint16_t length;                      /* longitud en bits          */
} HufCode;

/* ------------------------------------------------------------------ */
/*  Reporte de progreso compartido (lo actualizan los workers,        */
/*  lo lee el hilo/UI que muestra la barra). Protegido por mutex.     */
/* ------------------------------------------------------------------ */
typedef struct {
    pthread_mutex_t mtx;
    uint32_t total_bloques;
    uint32_t bloques_hechos;
    int      terminado;       /* 1 cuando toda la operacion finalizo  */
} HufProgress;

/* ================================================================== */
/*  API publica                                                       */
/* ================================================================== */

/* --- Arbol y codigos (huffman.c) --- */
HufNode *huf_build_tree(const uint64_t freq[HUF_NSYM]);
void     huf_free_tree(HufNode *root);
/* Genera la tabla de codigos recorriendo el arbol. */
void     huf_build_codes(const HufNode *root, HufCode codes[HUF_NSYM]);

/* --- Conteo de frecuencias concurrente (freq_parallel.c) --- */
/* Lee 'path' con 'nthreads' hilos y acumula las frecuencias globales.
 * Devuelve 0 en exito, -1 en error. Rellena 'freq' y '*filesize'.    */
int huf_count_frequencies(const char *path, int nthreads,
                          uint64_t freq[HUF_NSYM], uint64_t *filesize);

/* --- Pool de hilos + cola sincronizada (worker_pool.c) --- */
typedef struct WorkerPool WorkerPool;
typedef void (*huf_task_fn)(void *arg);

WorkerPool *pool_create(int nthreads);
void        pool_submit(WorkerPool *p, huf_task_fn fn, void *arg);
void        pool_wait(WorkerPool *p);     /* espera a que vacie la cola */
void        pool_destroy(WorkerPool *p);  /* join limpio de todos       */

/* --- Progreso (progress.c) --- */
void huf_progress_init(HufProgress *pg, uint32_t total);
void huf_progress_step(HufProgress *pg);          /* +1 bloque hecho    */
void huf_progress_finish(HufProgress *pg);
/* Lanza un hilo que imprime la barra hasta que terminado==1.          */
pthread_t huf_progress_spawn_printer(HufProgress *pg);

/* --- Compresion / Descompresion (compress.c / decompress.c) --- */
int huf_compress(const char *src, const char *dst, int nthreads);
int huf_decompress(const char *src, const char *dst, int nthreads);

#endif /* HUFFMAN_H */
