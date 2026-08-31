/**
 * ====================================================================================
 *  editor_core.c  -  NUCLEO DEL EDITOR  (Integrante 1)
 * ====================================================================================
 *  Responsabilidades de este modulo:
 *    - Ciclo de vida del descriptor de archivo: open(2) / close(2).
 *    - Construccion del INDICE DE LINEAS leyendo el archivo por bloques con read(2).
 *    - Comandos base: o (abrir), p (imprimir), a (anexar), q (salir).
 *    - Primitivas de desplazamiento de bytes en disco (insertar/borrar) que utilizan
 *      lseek(2), read(2), write(2) y ftruncate(2) apoyandose en un buffer dinamico.
 *    - Bucle REPL interactivo del editor.
 * ====================================================================================
 */

#include "shell.h"
#include "editor.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* Trazado estilo strace activo por defecto: el proyecto es didactico y la rubrica
   valora hacer visible que syscall se ejecuta detras de cada comando. */
int ed_traza = 1;

/* Macros de trazado condicional construidas sobre las macros del shell de clase. */
#define ED_LOG(nombre, fmt, ...)  do { if (ed_traza) LOG_SYSCALL(nombre, fmt, ##__VA_ARGS__); } while (0)
#define ED_LOG_OK(r)              do { if (ed_traza) LOG_SYSCALL_RESULT(r); } while (0)
#define ED_LOG_ERR()              do { if (ed_traza) LOG_SYSCALL_ERROR(strerror(errno)); } while (0)

/**
 * ------------------------------------------------------------------------------------
 * ed_fallo: reporte de error uniforme.
 * Usa perror(3) para traducir errno al texto del sistema, tal como exige la rubrica
 * de manejo de errores, y lo envuelve en color rojo para destacarlo en la terminal.
 * ------------------------------------------------------------------------------------
 */
void ed_fallo(const char *contexto)
{
    fprintf(stderr, COLOR_ERROR);
    perror(contexto);
    fprintf(stderr, COLOR_RESET);
    fflush(stderr);
}

/**
 * ------------------------------------------------------------------------------------
 * ed_init: deja la estructura Editor en un estado consistente y vacio.
 * ------------------------------------------------------------------------------------
 */
void ed_init(Editor *ed)
{
    memset(ed, 0, sizeof(Editor));
    ed->fd = -1;
    ed->abierto = 0;
    ed->lineas = NULL;
    ed->n_lineas = 0;
    ed->cap_lineas = 0;
    ed->tam = 0;
    ed->clip_frente = 0;
    ed->clip_n = 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_leer_exacto: lee EXACTAMENTE n bytes desde el desplazamiento 'desde'.
 * read(2) puede retornar menos bytes de los pedidos (lecturas cortas); por eso se
 * itera hasta completar n. Retorna 0 en exito, -1 en error.
 * ------------------------------------------------------------------------------------
 */
int ed_leer_exacto(int fd, off_t desde, void *buf, size_t n)
{
    char  *p = (char *)buf;
    size_t leidos = 0;

    if (lseek(fd, desde, SEEK_SET) == (off_t)-1) {
        ed_fallo("lseek");
        return -1;
    }
    while (leidos < n) {
        ssize_t r = read(fd, p + leidos, n - leidos);
        if (r < 0) {
            if (errno == EINTR) continue;   /* Interrumpida por senal: reintentar */
            ed_fallo("read");
            return -1;
        }
        if (r == 0) break;                  /* EOF inesperado */
        leidos += (size_t)r;
    }
    return (leidos == n) ? 0 : -1;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_escribir_exacto: escribe EXACTAMENTE n bytes en la posicion actual del fd.
 * write(2) tambien admite escrituras parciales; se itera hasta agotar el buffer.
 * ------------------------------------------------------------------------------------
 */
int ed_escribir_exacto(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t escritos = 0;

    while (escritos < n) {
        ssize_t w = write(fd, p + escritos, n - escritos);
        if (w < 0) {
            if (errno == EINTR) continue;
            ed_fallo("write");
            return -1;
        }
        escritos += (size_t)w;
    }
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_push_linea: agrega una entrada al indice, duplicando la capacidad si hace falta.
 * Estrategia de crecimiento amortizado O(1) con realloc().
 * ------------------------------------------------------------------------------------
 */
static int ed_push_linea(Editor *ed, off_t offset, size_t longitud, int con_salto)
{
    if (ed->n_lineas == ed->cap_lineas) {
        size_t nueva = (ed->cap_lineas == 0) ? ED_CAP_INICIAL : ed->cap_lineas * 2;
        Linea *tmp = (Linea *)realloc(ed->lineas, nueva * sizeof(Linea));
        if (tmp == NULL) {
            ed_fallo("realloc del indice de lineas");
            return -1;
        }
        ed->lineas = tmp;
        ed->cap_lineas = nueva;
    }
    ed->lineas[ed->n_lineas].offset    = offset;
    ed->lineas[ed->n_lineas].longitud  = longitud;
    ed->lineas[ed->n_lineas].con_salto = con_salto;
    ed->n_lineas++;
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_indexar: RECONSTRUYE EL INDICE DE LINEAS.
 *
 * Recorre el archivo completo con read(2) en bloques de ED_BLOQUE_IO bytes buscando
 * el separador '\n'. Por cada salto de linea encontrado registra (offset, longitud).
 *
 * Decision de diseno: el indice se reconstruye despues de cada operacion que modifica
 * el archivo. Es O(N) en bytes, pero garantiza que jamas se trabaje con offsets
 * obsoletos (la fuente de verdad siempre es el disco, no una copia en RAM).
 * ------------------------------------------------------------------------------------
 */
int ed_indexar(Editor *ed)
{
    char    buf[ED_BLOQUE_IO];
    ssize_t n;
    off_t   absoluto = 0;
    off_t   inicio   = 0;
    size_t  largo    = 0;

    ed->n_lineas = 0;

    if (lseek(ed->fd, 0, SEEK_SET) == (off_t)-1) {
        ed_fallo("lseek");
        return -1;
    }

    while ((n = read(ed->fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (ed_push_linea(ed, inicio, largo, 1) < 0) return -1;
                inicio = absoluto + i + 1;
                largo  = 0;
            } else {
                largo++;
            }
        }
        absoluto += n;
    }
    if (n < 0) {
        ed_fallo("read");
        return -1;
    }
    /* Ultima linea sin '\n' final (archivo que no termina en salto de linea). */
    if (largo > 0) {
        if (ed_push_linea(ed, inicio, largo, 0) < 0) return -1;
    }

    ed->tam = absoluto;
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_valida_linea: traduce el numero de linea 1-based del usuario al indice 0-based
 * interno, verificando el rango. Centraliza el caso borde de "linea fuera de rango".
 * ------------------------------------------------------------------------------------
 */
int ed_valida_linea(Editor *ed, long n, size_t *idx)
{
    if (ed->n_lineas == 0) {
        fprintf(stderr, COLOR_ERROR "El archivo esta vacio: no hay lineas.\n" COLOR_RESET);
        return -1;
    }
    if (n < 1 || (size_t)n > ed->n_lineas) {
        fprintf(stderr, COLOR_ERROR "Linea %ld fuera de rango (el archivo tiene %zu lineas).\n" COLOR_RESET,
                n, ed->n_lineas);
        return -1;
    }
    *idx = (size_t)(n - 1);
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_leer_linea: reserva un buffer dinamico y trae del disco el contenido de la
 * linea 'idx' con un unico lseek(2) + read(2) gracias al indice.
 * El llamador es responsable de hacer free() del buffer devuelto.
 * ------------------------------------------------------------------------------------
 */
int ed_leer_linea(Editor *ed, size_t idx, char **salida)
{
    size_t len = ed->lineas[idx].longitud;
    char  *buf = (char *)malloc(len + 1);

    if (buf == NULL) {
        ed_fallo("malloc de linea");
        return -1;
    }
    if (len > 0 && ed_leer_exacto(ed->fd, ed->lineas[idx].offset, buf, len) < 0) {
        free(buf);
        return -1;
    }
    buf[len] = '\0';
    *salida = buf;
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_abrir: comando 'o [archivo]'.
 * open(2) con O_RDWR|O_CREAT: lectura y escritura sobre el mismo descriptor, y
 * creacion si no existe con permisos 0644 (rw-r--r--) filtrados por la umask.
 * NO se usa O_TRUNC: abrir un archivo existente jamas debe destruir su contenido.
 * ------------------------------------------------------------------------------------
 */
int ed_abrir(Editor *ed, const char *ruta)
{
    int fd;

    if (ed->abierto) {
        /* Cerrar el buffer anterior antes de abrir otro (gestion estricta de FDs). */
        ed_cerrar(ed);
    }

    ED_LOG("open", "\"%s\", O_RDWR|O_CREAT, 0644", ruta);
    fd = open(ruta, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        ED_LOG_ERR();
        ed_fallo("open");
        return -1;
    }
    ED_LOG_OK(fd);

    ed->fd = fd;
    ed->abierto = 1;
    strncpy(ed->ruta, ruta, PATH_MAX - 1);
    ed->ruta[PATH_MAX - 1] = '\0';

    if (ed_indexar(ed) < 0) {
        close(fd);
        ed->fd = -1;
        ed->abierto = 0;
        return -1;
    }

    printf(COLOR_RESULT "Archivo '%s' abierto (fd=%d, %lld bytes, %zu lineas).\n" COLOR_RESET,
           ed->ruta, ed->fd, (long long)ed->tam, ed->n_lineas);
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_cerrar: comando 'q' (parte de cierre).
 * Libera el indice, vacia el portapapeles y cierra el descriptor. El objetivo es
 * salir SIN FUGAS DE MEMORIA ni descriptores huerfanos, como pide el enunciado.
 * ------------------------------------------------------------------------------------
 */
int ed_cerrar(Editor *ed)
{
    int r = 0;

    if (ed->lineas != NULL) {
        free(ed->lineas);
        ed->lineas = NULL;
    }
    ed->n_lineas = 0;
    ed->cap_lineas = 0;

    ed_clip_liberar(ed);

    if (ed->abierto && ed->fd >= 0) {
        ED_LOG("close", "%d", ed->fd);
        r = close(ed->fd);
        if (r < 0) {
            ED_LOG_ERR();
            ed_fallo("close");
        } else {
            ED_LOG_OK(r);
        }
    }
    ed->fd = -1;
    ed->abierto = 0;
    ed->tam = 0;
    ed->ruta[0] = '\0';
    return r;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_insertar_bytes: INSERCION EN MEDIO DEL ARCHIVO.
 *
 * Un archivo POSIX no tiene "insertar": write() sobreescribe. Para insertar n bytes
 * en la posicion 'pos' sin perder informacion se aplica la tecnica de
 * DESPLAZAMIENTO CON BUFFER DINAMICO DE COLA:
 *
 *   1. Se calcula la cola = [pos, EOF) y se reserva un buffer con malloc().
 *   2. Se lee la cola completa a memoria (lseek + read).
 *   3. Se reposiciona el cursor en 'pos' y se escriben los datos nuevos.
 *   4. Se reescribe la cola inmediatamente despues.
 *   5. free() del buffer.
 *
 * Coste: O(tamano de la cola) en memoria y en E/S. Se prefirio esta via sobre un
 * archivo temporal porque conserva el mismo inodo (y por tanto los enlaces duros,
 * permisos y metadatos originales del archivo).
 * ------------------------------------------------------------------------------------
 */
int ed_insertar_bytes(Editor *ed, off_t pos, const char *datos, size_t n)
{
    size_t cola_len;
    char  *cola = NULL;

    if (pos > ed->tam) pos = ed->tam;
    cola_len = (size_t)(ed->tam - pos);

    if (cola_len > 0) {
        cola = (char *)malloc(cola_len);
        if (cola == NULL) {
            ed_fallo("malloc del buffer de cola");
            return -1;
        }
        if (ed_leer_exacto(ed->fd, pos, cola, cola_len) < 0) {
            free(cola);
            return -1;
        }
    }

    ED_LOG("lseek", "%d, %lld, SEEK_SET", ed->fd, (long long)pos);
    if (lseek(ed->fd, pos, SEEK_SET) == (off_t)-1) {
        ED_LOG_ERR();
        ed_fallo("lseek");
        free(cola);
        return -1;
    }
    ED_LOG_OK(pos);

    ED_LOG("write", "%d, <%zu bytes nuevos>, %zu", ed->fd, n, n);
    if (ed_escribir_exacto(ed->fd, datos, n) < 0) {
        free(cola);
        return -1;
    }
    ED_LOG_OK(n);

    if (cola_len > 0) {
        ED_LOG("write", "%d, <cola desplazada>, %zu", ed->fd, cola_len);
        if (ed_escribir_exacto(ed->fd, cola, cola_len) < 0) {
            free(cola);
            return -1;
        }
        ED_LOG_OK(cola_len);
    }

    free(cola);
    ed->tam += (off_t)n;
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_borrar_bytes: ELIMINACION DE UN RANGO DE BYTES.
 *
 *   1. Se lee a memoria la cola posterior al rango: [pos+n, EOF).
 *   2. Se reescribe esa cola comenzando en 'pos' (compactacion).
 *   3. ftruncate(2) recorta el archivo al nuevo tamano, eliminando el residuo.
 *
 * Sin el paso 3 quedarian bytes "fantasma" al final del archivo: ftruncate es la
 * unica syscall que reduce el tamano de un archivo ya abierto.
 * ------------------------------------------------------------------------------------
 */
int ed_borrar_bytes(Editor *ed, off_t pos, size_t n)
{
    size_t cola_len;
    char  *cola = NULL;
    off_t  nuevo_tam;

    if (pos < 0 || pos >= ed->tam) return -1;
    if ((off_t)(pos + (off_t)n) > ed->tam) n = (size_t)(ed->tam - pos);

    cola_len = (size_t)(ed->tam - pos - (off_t)n);

    if (cola_len > 0) {
        cola = (char *)malloc(cola_len);
        if (cola == NULL) {
            ed_fallo("malloc del buffer de cola");
            return -1;
        }
        if (ed_leer_exacto(ed->fd, pos + (off_t)n, cola, cola_len) < 0) {
            free(cola);
            return -1;
        }

        ED_LOG("lseek", "%d, %lld, SEEK_SET", ed->fd, (long long)pos);
        if (lseek(ed->fd, pos, SEEK_SET) == (off_t)-1) {
            ED_LOG_ERR();
            ed_fallo("lseek");
            free(cola);
            return -1;
        }
        ED_LOG_OK(pos);

        ED_LOG("write", "%d, <cola compactada>, %zu", ed->fd, cola_len);
        if (ed_escribir_exacto(ed->fd, cola, cola_len) < 0) {
            free(cola);
            return -1;
        }
        ED_LOG_OK(cola_len);
        free(cola);
    }

    nuevo_tam = ed->tam - (off_t)n;
    ED_LOG("ftruncate", "%d, %lld", ed->fd, (long long)nuevo_tam);
    if (ftruncate(ed->fd, nuevo_tam) < 0) {
        ED_LOG_ERR();
        ed_fallo("ftruncate");
        return -1;
    }
    ED_LOG_OK(0);

    ed->tam = nuevo_tam;
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_imprimir: comando 'p [n]'.
 * Sin argumento imprime todo el archivo numerando las lineas; con argumento imprime
 * solo la linea n. En ambos casos el contenido se trae del disco con lseek+read
 * (nunca desde una copia cacheada), de modo que lo mostrado es el estado real.
 * ------------------------------------------------------------------------------------
 */
int ed_imprimir(Editor *ed, long n)
{
    char *texto = NULL;

    if (n < 0) {
        if (ed->n_lineas == 0) {
            printf(COLOR_INFO "(archivo vacio)\n" COLOR_RESET);
            return 0;
        }
        printf(COLOR_TITLE "--- %s (%zu lineas, %lld bytes) ---\n" COLOR_RESET,
               ed->ruta, ed->n_lineas, (long long)ed->tam);
        for (size_t i = 0; i < ed->n_lineas; i++) {
            if (ed_leer_linea(ed, i, &texto) < 0) return -1;
            printf(COLOR_PARAM "%4zu" COLOR_RESET " | %s\n", i + 1, texto);
            free(texto);
            texto = NULL;
        }
        printf(COLOR_TITLE "----------------------------------------\n" COLOR_RESET);
        return 0;
    }

    size_t idx;
    if (ed_valida_linea(ed, n, &idx) < 0) return -1;
    if (ed_leer_linea(ed, idx, &texto) < 0) return -1;
    printf(COLOR_PARAM "%4ld" COLOR_RESET " | %s\n", n, texto);
    free(texto);
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_anexar: comando 'a [texto]'.
 * Posiciona el cursor al final con lseek(fd, 0, SEEK_END) y escribe la linea nueva.
 * Caso borde tratado: si el archivo no termina en '\n', primero se escribe el salto
 * para no concatenar el texto nuevo dentro de la ultima linea existente.
 * ------------------------------------------------------------------------------------
 */
int ed_anexar(Editor *ed, const char *texto)
{
    size_t len = strlen(texto);
    off_t  fin;
    int    falta_salto = 0;

    if (ed->n_lineas > 0 && ed->lineas[ed->n_lineas - 1].con_salto == 0)
        falta_salto = 1;

    ED_LOG("lseek", "%d, 0, SEEK_END", ed->fd);
    fin = lseek(ed->fd, 0, SEEK_END);
    if (fin == (off_t)-1) {
        ED_LOG_ERR();
        ed_fallo("lseek");
        return -1;
    }
    ED_LOG_OK(fin);

    if (falta_salto) {
        ED_LOG("write", "%d, \"\\n\", 1", ed->fd);
        if (ed_escribir_exacto(ed->fd, "\n", 1) < 0) return -1;
        ED_LOG_OK(1);
        ed->tam += 1;
    }

    ED_LOG("write", "%d, \"%s\\n\", %zu", ed->fd, texto, len + 1);
    if (ed_escribir_exacto(ed->fd, texto, len) < 0) return -1;
    if (ed_escribir_exacto(ed->fd, "\n", 1) < 0) return -1;
    ED_LOG_OK(len + 1);

    ed->tam += (off_t)(len + 1);
    if (ed_indexar(ed) < 0) return -1;

    printf(COLOR_RESULT "Linea %zu anexada al final.\n" COLOR_RESET, ed->n_lineas);
    return 0;
}

/* ====================================================================================
 *  BUCLE INTERACTIVO (REPL) DEL EDITOR
 * ==================================================================================== */

/* Elimina espacios iniciales de una cadena y devuelve el puntero resultante. */
static char *ed_saltar_espacios(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Elimina el salto de linea final que deja fgets(). */
static void ed_quitar_salto(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r')) s[--l] = '\0';
}

/* Ayuda del editor. */
static void ed_ayuda(void)
{
    printf(COLOR_TITLE "\n--- Editor de texto POSIX (categoria 'edicion') ---\n" COLOR_RESET);
    printf("  " COLOR_PROMPT "o <archivo>" COLOR_RESET "        Abre o crea un archivo.            " COLOR_INFO "open(2)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "p [n]" COLOR_RESET "              Imprime todo o la linea n.         " COLOR_INFO "lseek(2), read(2)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "a <texto>" COLOR_RESET "          Anexa una linea al final.          " COLOR_INFO "lseek(2) SEEK_END, write(2)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "i <n> <texto>" COLOR_RESET "      Inserta texto como linea n.        " COLOR_INFO "lseek(2), read(2), write(2)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "d <n>" COLOR_RESET "              Borra la linea n.                  " COLOR_INFO "read(2), write(2), ftruncate(2)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "s <palabra>" COLOR_RESET "        Busca una palabra en el archivo.   " COLOR_INFO "lseek(2), read(2)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "m" COLOR_RESET "                  Metadatos del archivo abierto.     " COLOR_INFO "fstat(2)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "y <n>" COLOR_RESET "              Copia la linea n al portapapeles.  " COLOR_INFO "lseek(2), read(2), malloc(3)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "x [n]" COLOR_RESET "              Pega el portapapeles en la linea n. " COLOR_INFO "lseek(2), write(2)" COLOR_RESET "\n");
    printf("  " COLOR_PROMPT "c" COLOR_RESET "                  Muestra el estado del portapapeles.\n");
    printf("  " COLOR_PROMPT "t" COLOR_RESET "                  Conmuta el trazado de syscalls.\n");
    printf("  " COLOR_PROMPT "h" COLOR_RESET "                  Muestra esta ayuda.\n");
    printf("  " COLOR_PROMPT "q" COLOR_RESET "                  Cierra el descriptor y sale.       " COLOR_INFO "close(2)" COLOR_RESET "\n\n");
    printf(COLOR_INFO "  Nota: no existe comando 'guardar'. Toda edicion se persiste de inmediato\n");
    printf("  con write(2)/ftruncate(2) sobre el mismo inodo.\n\n" COLOR_RESET);
}

/**
 * ------------------------------------------------------------------------------------
 * ed_repl: bucle Read-Eval-Print del editor.
 * Es un REPL anidado: se invoca desde el shell de clase mediante el comando 'edi' y
 * devuelve el control al shell cuando el usuario escribe 'q' o presiona Ctrl+D.
 * La lectura de comandos usa fgets() sobre STDIN, unico uso permitido de la E/S
 * estandar segun el enunciado.
 * ------------------------------------------------------------------------------------
 */
int ed_repl(const char *archivo_inicial)
{
    Editor ed;
    char   linea[ED_MAX_ENTRADA];
    int    salir = 0;

    ed_init(&ed);

    printf(COLOR_TITLE "\n========================================================\n" COLOR_RESET);
    printf(COLOR_TITLE "   Editor de Texto CLI sobre syscalls POSIX (EAFITOS)\n" COLOR_RESET);
    printf(COLOR_INFO  "   Escribe 'h' para la ayuda, 'q' para volver al shell.\n" COLOR_RESET);
    printf(COLOR_TITLE "========================================================\n\n" COLOR_RESET);

    if (archivo_inicial != NULL && archivo_inicial[0] != '\0') {
        ed_abrir(&ed, archivo_inicial);
    }

    while (!salir) {
        if (ed.abierto)
            printf(COLOR_PROMPT "edi:%s> " COLOR_RESET, ed.ruta);
        else
            printf(COLOR_PROMPT "edi> " COLOR_RESET);
        fflush(stdout);

        if (fgets(linea, sizeof(linea), stdin) == NULL) {
            printf("\n");
            break;                       /* Ctrl+D (EOF) equivale a 'q' */
        }
        ed_quitar_salto(linea);

        char *p = ed_saltar_espacios(linea);
        if (*p == '\0') continue;        /* Linea vacia: ignorar */

        char cmd = *p;
        /* Los comandos son de una sola letra: 'pp' o 'abc' deben rechazarse. */
        if (p[1] != '\0' && p[1] != ' ' && p[1] != '\t') {
            fprintf(stderr, COLOR_ERROR "Comando '%s' no reconocido. Escribe 'h' para la ayuda.\n" COLOR_RESET, p);
            continue;
        }
        char *resto = ed_saltar_espacios(p + 1);

        /* Comandos que no requieren archivo abierto */
        if (cmd == 'q') { salir = 1; continue; }
        if (cmd == 'h') { ed_ayuda(); continue; }
        if (cmd == 't') {
            ed_traza = !ed_traza;
            printf(COLOR_INFO "Trazado de syscalls %s.\n" COLOR_RESET, ed_traza ? "ACTIVADO" : "DESACTIVADO");
            continue;
        }
        if (cmd == 'o') {
            if (*resto == '\0') {
                fprintf(stderr, COLOR_ERROR "Uso: o <archivo>\n" COLOR_RESET);
                continue;
            }
            ed_abrir(&ed, resto);
            continue;
        }

        /* A partir de aqui se exige un archivo abierto */
        if (!ed.abierto) {
            fprintf(stderr, COLOR_ERROR "No hay archivo abierto. Usa: o <archivo>\n" COLOR_RESET);
            continue;
        }

        switch (cmd) {
        case 'p': {
            long n = (*resto == '\0') ? -1 : strtol(resto, NULL, 10);
            ed_imprimir(&ed, n);
            break;
        }
        case 'a':
            if (*resto == '\0') {
                fprintf(stderr, COLOR_ERROR "Uso: a <texto>\n" COLOR_RESET);
                break;
            }
            ed_anexar(&ed, resto);
            break;
        case 'i': {
            char *fin = NULL;
            long  n = strtol(resto, &fin, 10);
            if (fin == resto) {
                fprintf(stderr, COLOR_ERROR "Uso: i <n> <texto>\n" COLOR_RESET);
                break;
            }
            char *texto = ed_saltar_espacios(fin);
            ed_insertar_linea(&ed, n, texto);
            break;
        }
        case 'd': {
            if (*resto == '\0') {
                fprintf(stderr, COLOR_ERROR "Uso: d <n>\n" COLOR_RESET);
                break;
            }
            ed_borrar_linea(&ed, strtol(resto, NULL, 10));
            break;
        }
        case 's':
            if (*resto == '\0') {
                fprintf(stderr, COLOR_ERROR "Uso: s <palabra>\n" COLOR_RESET);
                break;
            }
            ed_buscar(&ed, resto);
            break;
        case 'm':
            ed_metadatos(&ed);
            break;
        case 'y':
            if (*resto == '\0') {
                fprintf(stderr, COLOR_ERROR "Uso: y <n>\n" COLOR_RESET);
                break;
            }
            ed_copiar(&ed, strtol(resto, NULL, 10));
            break;
        case 'x': {
            long n = (*resto == '\0') ? -1 : strtol(resto, NULL, 10);
            ed_pegar(&ed, n);
            break;
        }
        case 'c':
            ed_clip_estado(&ed);
            break;
        default:
            fprintf(stderr, COLOR_ERROR "Comando '%c' no reconocido. Escribe 'h' para la ayuda.\n" COLOR_RESET, cmd);
            break;
        }
    }

    ed_cerrar(&ed);
    printf(COLOR_INFO "Editor cerrado. Regresando al shell.\n" COLOR_RESET);
    return 0;
}
