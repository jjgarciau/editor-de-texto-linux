/**
 * ====================================================================================
 *  cat_edicion.c  -  CATEGORIA 'edicion' DEL SHELL EAFITOS
 * ====================================================================================
 *  PUNTO DE INTEGRACION ENTRE EL SHELL DE CLASE Y EL EDITOR DE TEXTO.
 *
 *  Decision arquitectonica (justificada en el documento de sustentacion):
 *  se creo una CATEGORIA NUEVA llamada 'edicion' en lugar de colgar el editor de la
 *  categoria 'datos' existente. Razon:
 *
 *    - Los comandos de 'datos' (d_create, d_read, d_info, d_copy) son ATOMICOS y SIN
 *      ESTADO: abren, operan y cierran el descriptor dentro de una sola invocacion.
 *    - El editor es CONVERSACIONAL Y CON ESTADO: mantiene un descriptor abierto, un
 *      indice de lineas y un portapapeles vivos entre comandos, y abre un REPL
 *      anidado con su propio prompt.
 *
 *  Mezclarlos romperia el contrato semantico que el shell le promete al estudiante en
 *  'help datos'. La categoria 'edicion' documenta esa diferencia de forma explicita.
 *
 *  Mecanismo elegido: ENLACE EN EL MISMO PROCESO (llamada directa a ed_repl), no
 *  fork()+execvp(). Asi el editor comparte la tabla de descriptores y el trazado de
 *  syscalls del shell, que es justamente el valor pedagogico del proyecto; ademas se
 *  evita el coste de duplicar el espacio de direcciones para una herramienta que no
 *  necesita aislamiento.
 * ====================================================================================
 */

#include "shell.h"
#include "editor.h"

#include <stdio.h>
#include <string.h>

/**
 * ------------------------------------------------------------------------------------
 * cmd_edi: comando 'edi [archivo]' registrado en la tabla Command del shell.
 * Si recibe una ruta la abre de inmediato; si no, entra al editor sin archivo y el
 * usuario abre uno con 'o <archivo>'.
 * ------------------------------------------------------------------------------------
 */
int cmd_edi(int argc, char **argv)
{
    const char *archivo = NULL;

    if (argc > 2) {
        fprintf(stderr, COLOR_ERROR "Uso: edi [archivo]\n" COLOR_RESET);
        return 1;
    }
    if (argc == 2) {
        archivo = argv[1];
    }

    return ed_repl(archivo);
}
