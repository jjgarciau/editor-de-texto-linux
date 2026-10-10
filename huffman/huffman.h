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
 *  Huffman en la descompresion (correccion algoritmica -> salida identica).
 * ==========================================================================*/
#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>

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
/*  lo leen el hilo impresor de la CLI o el editor). Protegido por    */
/*  mutex; 'cambio' avisa cada avance -> el lector duerme en          */
/*  pthread_cond_wait en lugar de consultar en bucle (sin polling).   */
/* ------------------------------------------------------------------ */
typedef enum {
    HUF_FASE_INICIO = 0,
    HUF_FASE_FRECUENCIAS,     /* conteo concurrente de frecuencias    */
    HUF_FASE_BLOQUES,         /* codificacion/decodificacion en pool  */
    HUF_FASE_FIN
} HufFase;

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cambio;   /* broadcast en cada avance o al final  */
    uint32_t total_bloques;
    uint32_t bloques_hechos;
    HufFase  fase;
    int      terminado;       /* 1 cuando toda la operacion finalizo  */
} HufProgress;

/* ------------------------------------------------------------------ */
/*  Opciones de una operacion (todas opcionales: NULL = sin uso).     */
/*  Permiten integrar el compresor al editor como tarea de fondo.     */
/* ------------------------------------------------------------------ */
typedef struct {
    HufProgress           *pg;      /* progreso compartido                 */
    volatile sig_atomic_t *cancel;  /* !=0 -> abortar (senal o comando 'k') */
    /* Se invoca UNA vez cuando el archivo de entrada ya fue leido por
       completo. El editor lo usa para liberar el bloqueo del archivo lo
       antes posible (region critica minima).                          */
    void (*fuente_leida)(void *arg);
    void  *fuente_arg;
} HufOpts;

/* Lectura ATOMICA de la bandera de cancelacion: la escribe otro hilo
   (comando 'k' del editor) o un manejador de senal, y la leen los
   workers. La carga atomica evita una condicion de carrera formal.  */
#define HUF_CANCELADO(c) ((c) != NULL && __atomic_load_n((c), __ATOMIC_RELAXED) != 0)

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
/* Igual, pero los hilos abortan si *cancel != 0 (puede ser NULL). */
int huf_count_frequencies_ex(const char *path, int nthreads,
                             uint64_t freq[HUF_NSYM], uint64_t *filesize,
                             volatile sig_atomic_t *cancel);

/* --- Pool de hilos + cola sincronizada (worker_pool.c) --- */
typedef struct WorkerPool WorkerPool;
typedef void (*huf_task_fn)(void *arg);

WorkerPool *pool_create(int nthreads);  /* NULL si falla */
int         pool_submit(WorkerPool *p, huf_task_fn fn, void *arg); /* 0 ok, -1 */
void        pool_wait(WorkerPool *p);     /* espera a que vacie la cola */
void        pool_destroy(WorkerPool *p);  /* join limpio de todos       */

/* --- Progreso (progress.c) --- */
void huf_progress_init(HufProgress *pg, uint32_t total);
void huf_progress_destroy(HufProgress *pg);
void huf_progress_set_fase(HufProgress *pg, HufFase fase, uint32_t total);
void huf_progress_step(HufProgress *pg);          /* +1 bloque hecho    */
void huf_progress_finish(HufProgress *pg);
/* Copia consistente del estado (bajo el mutex). */
void huf_progress_snapshot(HufProgress *pg, uint32_t *hechos, uint32_t *total,
                           HufFase *fase, int *terminado);
/* Duerme (pthread_cond_wait) hasta que 'hechos' cambie respecto a
   'visto' o la operacion termine. Devuelve el nuevo valor.           */
uint32_t huf_progress_wait(HufProgress *pg, uint32_t visto, int *terminado);
/* Dibuja "[#####-----]  52% (k/n bloques)" en 'out' (con \r).        */
void huf_progress_draw(FILE *out, uint32_t hechos, uint32_t total, HufFase fase);
/* Lanza un hilo que imprime la barra por stderr hasta terminado==1.
   Devuelve 0 si lo creo (y deja su id en *tid), -1 si fallo.          */
int huf_progress_spawn_printer(HufProgress *pg, pthread_t *tid);

/* --- E/S robusta (huf_io.c): reintenta EINTR y transferencias parciales --- */
int     huf_write_all(int fd, const void *buf, size_t n);   /* 0 ok, -1 error */
ssize_t huf_read_full(int fd, void *buf, size_t n);         /* bytes leidos o -1 */

/* --- Compresion / Descompresion (compress.c / decompress.c) --- */
/* Devuelven 0 en exito, -1 en error, 1 si fue cancelada. En error o
   cancelacion se borra el archivo de salida parcial.                 */
int huf_compress(const char *src, const char *dst, int nthreads);
int huf_decompress(const char *src, const char *dst, int nthreads);
int huf_compress_ex(const char *src, const char *dst, int nthreads, const HufOpts *op);
int huf_decompress_ex(const char *src, const char *dst, int nthreads, const HufOpts *op);

#endif /* HUFFMAN_H */
