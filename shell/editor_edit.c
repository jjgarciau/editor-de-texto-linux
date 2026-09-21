/**
 * ====================================================================================
 *  editor_edit.c  -  EDICION ARBITRARIA Y BUSQUEDA  (Integrante 2)
 * ====================================================================================
 *  Requisito acumulativo de equipos de 2 integrantes en adelante:
 *    - i [n] [texto] : insercion arbitraria de una linea en la posicion n.
 *    - d [n]         : borrado de la linea n compactando el archivo.
 *    - s [palabra]   : busqueda simple con reporte de linea y columna.
 *
 *  Reto tecnico cubierto: "Manejo avanzado de lseek y manipulacion de buffers
 *  dinamicos en memoria (malloc/free) para no perder datos al desplazar bytes".
 *  Ese desplazamiento vive en ed_insertar_bytes()/ed_borrar_bytes() (editor_core.c)
 *  y aqui se traduce el concepto de "linea" a un rango de bytes concreto.
 * ====================================================================================
 */

#include "shell.h"
#include "editor.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

/**
 * ------------------------------------------------------------------------------------
 * ed_insertar_linea: comando 'i [n] [texto]'.
 *
 * Traduccion linea -> bytes: insertar ANTES de la linea n significa escribir en el
 * offset donde esa linea comienza (indice[n-1].offset). El texto se compone en un
 * buffer dinamico como "texto\n" y se entrega a ed_insertar_bytes(), que desplaza
 * toda la cola posterior sin perder un solo byte.
 *
 * Casos borde tratados:
 *   - n == n_lineas + 1  -> equivale a anexar al final (se delega en ed_anexar).
 *   - archivo vacio      -> cualquier n valido se resuelve como anexado.
 *   - n fuera de rango   -> error explicito, el archivo no se toca.
 * ------------------------------------------------------------------------------------
 */
int ed_insertar_linea(Editor *ed, long n, const char *texto)
{
    size_t idx;
    size_t len;
    char  *bloque;
    off_t  pos;

    if (texto == NULL) texto = "";

    /* Insertar justo despues de la ultima linea == anexar. */
    if (ed->n_lineas == 0 || (size_t)n == ed->n_lineas + 1) {
        if (n < 1) {
            fprintf(stderr, COLOR_ERROR "El numero de linea debe ser >= 1.\n" COLOR_RESET);
            return -1;
        }
        return ed_anexar(ed, texto);
    }

    if (ed_valida_linea(ed, n, &idx) < 0) return -1;

    len = strlen(texto);
    /* Buffer dinamico que contiene la linea completa con su salto de linea. */
    bloque = (char *)malloc(len + 2);
    if (bloque == NULL) {
        ed_fallo("malloc de la linea a insertar");
        return -1;
    }
    memcpy(bloque, texto, len);
    bloque[len] = '\n';
    bloque[len + 1] = '\0';

    pos = ed->lineas[idx].offset;

    printf(COLOR_INFO "Insertando %zu bytes en el offset %lld (desplazando %lld bytes de cola).\n" COLOR_RESET,
           len + 1, (long long)pos, (long long)(ed->tam - pos));

    if (ed_insertar_bytes(ed, pos, bloque, len + 1) < 0) {
        free(bloque);
        return -1;
    }
    free(bloque);

    if (ed_indexar(ed) < 0) return -1;

    printf(COLOR_RESULT "Texto insertado como linea %ld. El archivo tiene ahora %zu lineas.\n" COLOR_RESET,
           n, ed->n_lineas);
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_borrar_linea: comando 'd [n]'.
 *
 * El rango de bytes a eliminar es [offset, offset + longitud + 1) donde el +1
 * corresponde al '\n' terminador. Tras compactar, ftruncate(2) recorta el residuo.
 *
 * Caso borde tratado: si la linea es la ultima y NO termina en '\n', se elimina
 * ademas el '\n' de la linea anterior; de lo contrario el archivo quedaria con un
 * salto de linea colgante que se leeria como una linea vacia adicional.
 * ------------------------------------------------------------------------------------
 */
int ed_borrar_linea(Editor *ed, long n)
{
    size_t idx;
    off_t  pos;
    size_t len;

    if (ed_valida_linea(ed, n, &idx) < 0) return -1;

    pos = ed->lineas[idx].offset;
    len = ed->lineas[idx].longitud + (ed->lineas[idx].con_salto ? 1 : 0);

    if (ed->lineas[idx].con_salto == 0 && idx > 0) {
        /* Absorber tambien el '\n' de la linea previa. */
        pos -= 1;
        len += 1;
    }

    printf(COLOR_INFO "Eliminando %zu bytes desde el offset %lld (compactando %lld bytes de cola).\n" COLOR_RESET,
           len, (long long)pos, (long long)(ed->tam - pos - (off_t)len));

    if (ed_borrar_bytes(ed, pos, len) < 0) return -1;
    if (ed_indexar(ed) < 0) return -1;

    printf(COLOR_RESULT "Linea %ld eliminada. El archivo tiene ahora %zu lineas (%lld bytes).\n" COLOR_RESET,
           n, ed->n_lineas, (long long)ed->tam);
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * ed_buscar: comando 's [palabra]'.
 *
 * Recorre el indice y, por cada linea, trae su contenido con un lseek(2)+read(2)
 * puntual y aplica strstr(). Reporta numero de linea y columna (1-based) de cada
 * coincidencia, incluyendo ocurrencias multiples dentro de la misma linea.
 *
 * Alternativa descartada: cargar el archivo entero en un unico buffer. Se prefirio
 * el recorrido por lineas para que el consumo de memoria dependa de la linea mas
 * larga y no del tamano total del archivo.
 * ------------------------------------------------------------------------------------
 */
int ed_buscar(Editor *ed, const char *palabra)
{
    char  *texto = NULL;
    size_t coincidencias = 0;

    if (palabra == NULL || *palabra == '\0') {
        fprintf(stderr, COLOR_ERROR "Uso: s <palabra>\n" COLOR_RESET);
        return -1;
    }
    if (ed->n_lineas == 0) {
        printf(COLOR_INFO "El archivo esta vacio: nada que buscar.\n" COLOR_RESET);
        return 0;
    }

    printf(COLOR_TITLE "--- Buscando \"%s\" en %s ---\n" COLOR_RESET, palabra, ed->ruta);

    for (size_t i = 0; i < ed->n_lineas; i++) {
        if (ed_leer_linea(ed, i, &texto) < 0) return -1;

        char *desde = texto;
        char *hit;
        while ((hit = strstr(desde, palabra)) != NULL) {
            coincidencias++;
            printf("  " COLOR_PARAM "linea %zu" COLOR_RESET ", columna " COLOR_PARAM "%ld" COLOR_RESET " | %s\n",
                   i + 1, (long)(hit - texto) + 1, texto);
            desde = hit + 1;   /* Permite detectar solapamientos y repeticiones */
        }
        free(texto);
        texto = NULL;
    }

    if (coincidencias == 0)
        printf(COLOR_INFO "Sin coincidencias para \"%s\".\n" COLOR_RESET, palabra);
    else
        printf(COLOR_RESULT "%zu coincidencia(s) encontradas.\n" COLOR_RESET, coincidencias);

    return (int)coincidencias;
}
