/* ============================================================================
 *  huf_io.c  -  E/S robusta sobre read(2)/write(2)
 * ----------------------------------------------------------------------------
 *  read() y write() pueden transferir MENOS bytes de los pedidos (transferencia
 *  parcial) o ser interrumpidas por una senal (EINTR). Estas dos funciones
 *  reintentan hasta completar, igual que ed_escribir_exacto en el editor del
 *  Parcial 1. Se usan en toda la E/S del compresor.
 * ==========================================================================*/
#include "huffman.h"
#include <errno.h>
#include <unistd.h>

int huf_write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;   /* interrumpida: reintentar */
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

ssize_t huf_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t   got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;                  /* EOF */
        got += (size_t)r;
    }
    return (ssize_t)got;
}
