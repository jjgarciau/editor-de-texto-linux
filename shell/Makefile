# ====================================================================================
#  Makefile - Shell educativo EAFITOS + Editor de texto CLI (SO2026B)
# ====================================================================================
#  Targets:
#    make          -> compila el shell (eafitOS) y el editor autonomo (edi)
#    make eafitOS  -> solo el shell con el editor integrado
#    make edi      -> solo el binario autonomo del editor
#    make run      -> compila y ejecuta el shell
#    make pruebas  -> ejecuta el script de pruebas automatizadas
#    make clean    -> elimina objetos y binarios
# ====================================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE

SHELL_BIN  = eafitOS
EDITOR_BIN = edi

# Modulos del editor, reutilizados por ambos binarios (biblioteca comun)
EDITOR_SRCS = editor_core.c editor_edit.c editor_meta.c
EDITOR_OBJS = $(EDITOR_SRCS:.c=.o)

# Modulos del shell de clase + la nueva categoria 'edicion'
SHELL_SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c \
             cat_novatos.c cat_edicion.c
SHELL_OBJS = $(SHELL_SRCS:.c=.o)

all: $(SHELL_BIN) $(EDITOR_BIN)

$(SHELL_BIN): $(SHELL_OBJS) $(EDITOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(EDITOR_BIN): edi_main.o $(EDITOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c shell.h editor.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(SHELL_BIN)
	./$(SHELL_BIN)

pruebas: $(EDITOR_BIN)
	bash pruebas_editor.sh

clean:
	rm -f $(SHELL_OBJS) $(EDITOR_OBJS) edi_main.o $(SHELL_BIN) $(EDITOR_BIN)

.PHONY: all run pruebas clean
