#ifndef EDITOR_H
#define EDITOR_H

/**
 * ====================================================================================
 *  EDITOR DE TEXTO CLI SOBRE LLAMADAS AL SISTEMA POSIX  (SO2026B - EAFIT)
 * ====================================================================================
 *  Este encabezado concentra TODAS las estructuras de datos del editor y los
 *  prototipos repartidos entre los tres modulos de implementacion:
 *
 *    editor_core.c  -> Integrante 1: apertura, indice de lineas, impresion, anexado,
 *                      primitivas de desplazamiento de bytes y bucle REPL.
 *    editor_edit.c  -> Integrante 2: insercion arbitraria, borrado de lineas y busqueda.
 *    editor_meta.c  -> Integrante 3: metadatos por fstat() y portapapeles secuencial.
 *
 *  RESTRICCION DE E/S (impuesta por el enunciado):
 *  Ningun acceso al archivo de texto usa la biblioteca estandar de alto nivel
 *  (fopen/fread/fwrite/fclose). Todo el acceso a disco se realiza exclusivamente con
 *  open(2), read(2), write(2), lseek(2), ftruncate(2), fstat(2) y close(2).
 *  printf()/fgets() se emplean unicamente sobre STDIN/STDOUT para la interfaz.
 * ====================================================================================
 */

#include <sys/types.h>
#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Tamano del bloque de lectura usado por el indexador (coincide con el tamano
   tipico de pagina/bloque del sistema de archivos para minimizar syscalls). */
#define ED_BLOQUE_IO    4096

/* Longitud maxima de una linea de comando leida desde STDIN. */
#define ED_MAX_ENTRADA  4096

/* Capacidad inicial del arreglo dinamico de lineas (crece duplicandose). */
#define ED_CAP_INICIAL  64

/* Profundidad maxima del portapapeles secuencial (cola FIFO). */
#define ED_CLIP_MAX     32

/**
 * ------------------------------------------------------------------------------------
 * struct Linea  -  Entrada del INDICE DE LINEAS
 * ------------------------------------------------------------------------------------
 * El editor NO mantiene el archivo completo en memoria. Mantiene unicamente un
 * "indice" liviano: por cada linea guarda donde empieza en el archivo y cuanto mide.
 * Con ese par (offset, longitud) cualquier operacion se resuelve con un lseek()
 * directo, en tiempo O(1), sin recorrer el archivo desde el inicio.
 *
 * Costo de memoria: 24 bytes por linea aprox., independiente del tamano del texto.
 */
typedef struct {
    off_t  offset;      /* Desplazamiento en bytes desde el inicio del archivo (para lseek) */
    size_t longitud;    /* Bytes de la linea SIN contar el '\n' terminador                  */
    int    con_salto;   /* 1 si la linea termina en '\n'; 0 si es la ultima linea sin salto  */
} Linea;

/**
 * ------------------------------------------------------------------------------------
 * struct EntradaClip  -  Elemento del portapapeles
 * ------------------------------------------------------------------------------------
 * Copia profunda (malloc) del contenido de una linea. Es una copia y no una
 * referencia al indice porque las ediciones posteriores invalidan los offsets.
 */
typedef struct {
    char  *texto;       /* Buffer dinamico con el contenido copiado (terminado en '\0') */
    size_t longitud;    /* Bytes utiles del buffer                                      */
} EntradaClip;

/**
 * ------------------------------------------------------------------------------------
 * struct Editor  -  ESTADO GLOBAL DE LA SESION DE EDICION
 * ------------------------------------------------------------------------------------
 * Agrupa el descriptor de archivo abierto, el indice dinamico de lineas y el
 * portapapeles. Se pasa por puntero a todas las operaciones: no hay variables
 * globales de estado, de modo que el editor podria manejar varios buffers en el
 * futuro simplemente instanciando varios Editor (escalabilidad prevista).
 */
typedef struct {
    int    fd;                      /* Descriptor devuelto por open(); -1 si no hay archivo */
    char   ruta[PATH_MAX];          /* Ruta del archivo actualmente abierto                 */
    int    abierto;                 /* Bandera de sesion activa                             */
    off_t  tam;                     /* Tamano actual del archivo en bytes                   */

    Linea *lineas;                  /* Arreglo dinamico (malloc/realloc) del indice          */
    size_t n_lineas;                /* Numero de lineas indexadas                            */
    size_t cap_lineas;              /* Capacidad reservada del arreglo                       */

    EntradaClip clip[ED_CLIP_MAX];  /* Portapapeles secuencial (cola circular FIFO)          */
    size_t clip_frente;             /* Indice del proximo elemento a pegar                   */
    size_t clip_n;                  /* Elementos actualmente encolados                       */
} Editor;

/* Bandera global de trazado estilo strace (comando 't' la conmuta). */
extern int ed_traza;

/* ====================================================================================
 * MODULO 1 - editor_core.c   (Integrante 1)
 * ==================================================================================== */
void   ed_init(Editor *ed);                                     /* Inicializa la estructura        */
int    ed_abrir(Editor *ed, const char *ruta);                  /* open(2) O_RDWR|O_CREAT           */
int    ed_cerrar(Editor *ed);                                   /* close(2) + liberacion de memoria */
int    ed_indexar(Editor *ed);                                  /* read(2) por bloques -> indice    */
int    ed_leer_linea(Editor *ed, size_t idx, char **salida);    /* lseek(2)+read(2) de una linea    */
int    ed_imprimir(Editor *ed, long n);                         /* Comando p [n]                    */
int    ed_anexar(Editor *ed, const char *texto);                /* Comando a [texto] (SEEK_END)     */
int    ed_repl(const char *archivo_inicial);                    /* Bucle interactivo del editor     */

/* Primitivas de bajo nivel compartidas por los tres modulos */
int    ed_leer_exacto(int fd, off_t desde, void *buf, size_t n);
int    ed_escribir_exacto(int fd, const void *buf, size_t n);
int    ed_insertar_bytes(Editor *ed, off_t pos, const char *datos, size_t n);
int    ed_borrar_bytes(Editor *ed, off_t pos, size_t n);
void   ed_fallo(const char *contexto);                          /* perror(3) coloreado              */
int    ed_valida_linea(Editor *ed, long n, size_t *idx);        /* Validacion de rango 1..n_lineas  */

/* ====================================================================================
 * MODULO 2 - editor_edit.c   (Integrante 2)
 * ==================================================================================== */
int    ed_insertar_linea(Editor *ed, long n, const char *texto); /* Comando i [n] [texto] */
int    ed_borrar_linea(Editor *ed, long n);                      /* Comando d [n]         */
int    ed_buscar(Editor *ed, const char *palabra);               /* Comando s [palabra]   */

/* ====================================================================================
 * MODULO 3 - editor_meta.c   (Integrante 3)
 * ==================================================================================== */
int    ed_metadatos(Editor *ed);                                 /* Comando m  (fstat)    */
int    ed_copiar(Editor *ed, long n);                            /* Comando y [n]         */
int    ed_pegar(Editor *ed, long n);                             /* Comando x [n]         */
void   ed_clip_liberar(Editor *ed);                              /* free() del portapapeles */
void   ed_clip_estado(Editor *ed);                               /* Muestra la cola         */

#endif /* EDITOR_H */
