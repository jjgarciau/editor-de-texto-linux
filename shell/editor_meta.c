/**
 * ====================================================================================
 *  editor_meta.c  -  METADATOS DEL INODO Y PORTAPAPELES  (Integrante 3)
 * ====================================================================================
 *  Requisito acumulativo del equipo de 3 integrantes:
 *    - m       : imprime tamano, permisos, inodo y fecha de modificacion.
 *    - y [n]   : copia la linea n al portapapeles.
 *    - x [n]   : pega el contenido del portapapeles en la linea n.
 *
 *  Reto tecnico cubierto: "Integracion de la system call fstat() para acceder a la
 *  metadata en los inodos. Gestion de un portapapeles secuencial local".
 *
 *  Se eligio fstat(2) y no stat(2): fstat opera sobre el descriptor ya abierto, de
 *  modo que informa del inodo REAL que el editor esta manipulando aunque la ruta
 *  haya sido renombrada o borrada durante la sesion (evita una condicion de carrera
 *  del tipo TOCTOU entre la ruta y el archivo).
 * ====================================================================================
 */

#include "shell.h"
#include "editor.h"

#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

extern int ed_traza;

/* Convierte st_mode en la cadena tipo "-rw-r--r--" que muestra ls -l. */
static void ed_permisos_texto(mode_t modo, char *destino)
{
    const char *rwx = "rwxrwxrwx";
    destino[0] = S_ISDIR(modo) ? 'd' : (S_ISLNK(modo) ? 'l' : '-');
    for (int i = 0; i < 9; i++)
        destino[i + 1] = (modo & (1 << (8 - i))) ? rwx[i] : '-';
    destino[10] = '\0';
}

/**
 * ------------------------------------------------------------------------------------
 * ed_metadatos: comando 'm'.
 * Una unica llamada a fstat(2) llena una struct stat con la informacion que el
 * kernel guarda en el inodo. De ahi se derivan tamano, permisos, numero de inodo,
 * dispositivo, enlaces duros, bloques asignados y las tres marcas de tiempo.
 * ------------------------------------------------------------------------------------
 */
int ed_metadatos(Editor *ed)
{
    struct stat st;
    char        permisos[11];
    char        fecha[64];
    struct tm  *tm_info;
    struct passwd *pw;
    struct group  *gr;

    if (ed_traza) LOG_SYSCALL("fstat", "%d, &st", ed->fd);
    if (fstat(ed->fd, &st) < 0) {
        if (ed_traza) LOG_SYSCALL_ERROR(strerror(errno));
        ed_fallo("fstat");
        return -1;
    }
    if (ed_traza) LOG_SYSCALL_RESULT(0);

    ed_permisos_texto(st.st_mode, permisos);

    printf(COLOR_TITLE "\n--- Metadatos del inodo (fstat) ---\n" COLOR_RESET);
    printf("  Ruta:              %s\n", ed->ruta);
    printf("  Descriptor (fd):   " COLOR_PARAM "%d" COLOR_RESET "\n", ed->fd);
    printf("  Numero de inodo:   " COLOR_PARAM "%lu" COLOR_RESET "\n", (unsigned long)st.st_ino);
    printf("  Dispositivo:       %lu\n", (unsigned long)st.st_dev);
    printf("  Tamano:            " COLOR_PARAM "%lld bytes" COLOR_RESET " (%zu lineas indexadas)\n",
           (long long)st.st_size, ed->n_lineas);
    printf("  Permisos:          " COLOR_PARAM "%s" COLOR_RESET " (octal %o)\n",
           permisos, (unsigned)(st.st_mode & 07777));
    printf("  Enlaces duros:     %lu\n", (unsigned long)st.st_nlink);

    pw = getpwuid(st.st_uid);
    gr = getgrgid(st.st_gid);
    printf("  Propietario:       %u (%s)\n", (unsigned)st.st_uid, pw ? pw->pw_name : "?");
    printf("  Grupo:             %u (%s)\n", (unsigned)st.st_gid, gr ? gr->gr_name : "?");
    printf("  Bloques de 512B:   %lld (tam. bloque E/S: %ld)\n",
           (long long)st.st_blocks, (long)st.st_blksize);

    tm_info = localtime(&st.st_mtime);
    strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("  Modificacion:      " COLOR_PARAM "%s" COLOR_RESET "\n", fecha);

    tm_info = localtime(&st.st_atime);
    strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("  Ultimo acceso:     %s\n", fecha);

    tm_info = localtime(&st.st_ctime);
    strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("  Cambio de inodo:   %s\n", fecha);
    printf(COLOR_TITLE "-----------------------------------\n\n" COLOR_RESET);

    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * PORTAPAPELES SECUENCIAL
 * ------------------------------------------------------------------------------------
 * Implementado como una COLA CIRCULAR FIFO de copias profundas (malloc) sobre el
 * arreglo fijo Editor.clip[ED_CLIP_MAX].
 *
 * Se guarda una COPIA del texto y no una referencia (offset) porque cualquier
 * insercion o borrado posterior desplaza los bytes y dejaria el offset invalido.
 * El comportamiento es secuencial: varios 'y' encolan en orden y cada 'x' consume
 * el elemento mas antiguo, replicando el flujo natural de "copiar varias lineas y
 * pegarlas una tras otra".
 * ------------------------------------------------------------------------------------
 */

/* Comando 'y [n]': copia la linea n al final de la cola. */
int ed_copiar(Editor *ed, long n)
{
    size_t idx, ranura;
    char  *texto = NULL;

    if (ed_valida_linea(ed, n, &idx) < 0) return -1;

    if (ed->clip_n == ED_CLIP_MAX) {
        fprintf(stderr, COLOR_ERROR "Portapapeles lleno (%d entradas). Pega algo con 'x' antes de copiar mas.\n" COLOR_RESET,
                ED_CLIP_MAX);
        return -1;
    }
    if (ed_leer_linea(ed, idx, &texto) < 0) return -1;

    ranura = (ed->clip_frente + ed->clip_n) % ED_CLIP_MAX;
    ed->clip[ranura].texto    = texto;              /* Copia profunda ya reservada con malloc */
    ed->clip[ranura].longitud = strlen(texto);
    ed->clip_n++;

    printf(COLOR_RESULT "Linea %ld copiada al portapapeles (%zu bytes). Elementos en cola: %zu.\n" COLOR_RESET,
           n, ed->clip[ranura].longitud, ed->clip_n);
    return 0;
}

/**
 * Comando 'x [n]': extrae el elemento mas antiguo de la cola y lo inserta como
 * linea n. Sin argumento (n < 0) se pega al final del archivo.
 * Caso borde tratado: portapapeles vacio -> mensaje de error, archivo intacto.
 */
int ed_pegar(Editor *ed, long n)
{
    char  *texto;
    int    r;

    if (ed->clip_n == 0) {
        fprintf(stderr, COLOR_ERROR "El portapapeles esta vacio. Copia una linea con 'y <n>' primero.\n" COLOR_RESET);
        return -1;
    }

    texto = ed->clip[ed->clip_frente].texto;

    if (n < 0) {
        r = ed_anexar(ed, texto);
    } else {
        r = ed_insertar_linea(ed, n, texto);
    }

    if (r < 0) {
        /* Fallo la escritura: el elemento NO se consume, para no perder la copia. */
        return -1;
    }

    free(texto);
    ed->clip[ed->clip_frente].texto = NULL;
    ed->clip[ed->clip_frente].longitud = 0;
    ed->clip_frente = (ed->clip_frente + 1) % ED_CLIP_MAX;
    ed->clip_n--;

    printf(COLOR_RESULT "Contenido pegado. Quedan %zu elemento(s) en el portapapeles.\n" COLOR_RESET, ed->clip_n);
    return 0;
}

/* Comando 'c': muestra el contenido pendiente del portapapeles sin consumirlo. */
void ed_clip_estado(Editor *ed)
{
    printf(COLOR_TITLE "--- Portapapeles secuencial (FIFO): %zu/%d ---\n" COLOR_RESET, ed->clip_n, ED_CLIP_MAX);
    for (size_t i = 0; i < ed->clip_n; i++) {
        size_t pos = (ed->clip_frente + i) % ED_CLIP_MAX;
        printf("  [%zu] %s\n", i + 1, ed->clip[pos].texto ? ed->clip[pos].texto : "(nulo)");
    }
    if (ed->clip_n == 0)
        printf(COLOR_INFO "  (vacio)\n" COLOR_RESET);
    printf(COLOR_TITLE "------------------------------------------\n" COLOR_RESET);
}

/* Libera toda la memoria del portapapeles: se invoca desde ed_cerrar() ('q'). */
void ed_clip_liberar(Editor *ed)
{
    for (size_t i = 0; i < ed->clip_n; i++) {
        size_t pos = (ed->clip_frente + i) % ED_CLIP_MAX;
        if (ed->clip[pos].texto != NULL) {
            free(ed->clip[pos].texto);
            ed->clip[pos].texto = NULL;
            ed->clip[pos].longitud = 0;
        }
    }
    ed->clip_frente = 0;
    ed->clip_n = 0;
}
