# ====================================================================================
#  Makefile - Compresor Huffman Concurrente (Parcial 2 - SO)
# ------------------------------------------------------------------------------------
#  Modulo INDEPENDIENTE del editor (carpeta shell/). No comparte objetos ni
#  binarios con el editor; se compila por separado.
#
#  Targets:
#    make          -> compila el binario 'huffman'
#    make test     -> compila y ejecuta la prueba de integridad (round-trip)
#    make clean    -> elimina objetos y binario
# ====================================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -pthread
LDFLAGS = -pthread

BIN  = huffman
SRCS = main.c huffman.c freq_parallel.c worker_pool.c progress.c \
       compress.c decompress.c huf_io.c
OBJS = $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c huffman.h
	$(CC) $(CFLAGS) -c $< -o $@

# Prueba de integridad: comprime y descomprime, luego compara con el original.
test: $(BIN)
	bash test_huffman.sh

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all test clean
