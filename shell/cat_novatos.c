#include "shell.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/**
 * ====================================================================================
 * COMANDO: eco <archivo.txt>
 * ====================================================================================
 * Comando pensado para usuarios novatos: abre un archivo de texto y muestra en
 * consola todo su contenido, leyéndolo por bloques hasta llegar al final (EOF).
 *
 * Syscalls explicadas:
 * 1. open(2): Abre el archivo en modo de solo lectura (O_RDONLY).
 * 2. read(2): Copia bytes del archivo al buffer en memoria. Se llama repetidamente
 *    hasta que retorna 0, lo cual indica que se alcanzó el final del archivo (EOF).
 * 3. close(2): Libera el descriptor de archivo una vez terminada la lectura.
 */
int cmd_eco(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, COLOR_ERROR "Uso: eco <archivo.txt>\n" COLOR_RESET);
        return 1;
    }
    const char *filename = argv[1];
    char buffer[512];
    ssize_t bytes_read;

    /* 1. LLAMADA AL SISTEMA: open */
    // LOG_SYSCALL("open", "\"%s\", O_RDONLY", filename);
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    // LOG_SYSCALL_RESULT(fd);

    printf(COLOR_TITLE "--- Contenido de '%s' ---\n" COLOR_RESET, filename);

    /* 2. LLAMADA AL SISTEMA: read (repetida hasta EOF) */
    while (1) {
        // LOG_SYSCALL("read", "%d, buffer, %zu", fd, sizeof(buffer) - 1);
        bytes_read = read(fd, buffer, sizeof(buffer) - 1);
        if (bytes_read == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            close(fd);
            return 1;
        }
        // LOG_SYSCALL_RESULT(bytes_read);
        if (bytes_read == 0) break; /* Fin de archivo (EOF) alcanzado */

        buffer[bytes_read] = '\0';
        printf("%s", buffer);
    }
    printf("\n" COLOR_TITLE "-----------------------------\n" COLOR_RESET);

    /* 3. LLAMADA AL SISTEMA: close */
    // LOG_SYSCALL("close", "%d", fd);
    int res = close(fd);
    if (res == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    // LOG_SYSCALL_RESULT(res);

    return 0;
}
