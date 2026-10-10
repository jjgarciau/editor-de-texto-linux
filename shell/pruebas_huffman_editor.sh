#!/usr/bin/env bash
# ====================================================================================
#  pruebas_huffman_editor.sh  -  PRUEBAS DE LA INTEGRACION HUFFMAN CON EL EDITOR
#  (Parcial 2 - Concurrencia y Sincronizacion)
# ------------------------------------------------------------------------------------
#  Verifica, usando el editor 'edi' alimentado por STDIN:
#    1. Compresion en 2do plano: el prompt vuelve de inmediato y muestra el progreso.
#    2. Condicion de carrera: una edicion mientras el compresor lee la fuente se
#       RECHAZA (pthread_rwlock_trywrlock) y el archivo no se corrompe.
#    3. Integridad: descomprimir(comprimir(X)) == X  (md5).
#    4. Cancelacion con 'k': no queda archivo de salida a medias.
#    5. Validaciones: salida == archivo abierto, entrada inexistente.
#
#  Uso:   bash pruebas_huffman_editor.sh      (o bien:  make pruebas-huf)
# ====================================================================================
set -u
BIN="./edi"
DIR="$(mktemp -d /tmp/pruebas_huf.XXXXXX)"
OK=0; FALLOS=0
VERDE="\033[1;32m"; ROJO="\033[1;31m"; AZUL="\033[1;36m"; RESET="\033[0m"

titulo() { echo -e "\n${AZUL}===== $1 =====${RESET}"; }
comprobar() {   # comprobar <descripcion> <obtenido> <esperado>
    if [ "$2" == "$3" ]; then echo -e "  ${VERDE}[OK]${RESET}  $1"; OK=$((OK+1));
    else echo -e "  ${ROJO}[FALLA]${RESET} $1 (esperado '$3', obtenido '$2')"; FALLOS=$((FALLOS+1)); fi
}
contiene() {    # contiene <descripcion> <texto> <patron>
    if echo "$2" | grep -q -- "$3"; then echo -e "  ${VERDE}[OK]${RESET}  $1"; OK=$((OK+1));
    else echo -e "  ${ROJO}[FALLA]${RESET} $1 (no aparece '$3')"; FALLOS=$((FALLOS+1)); fi
}
editor() { { echo "t"; cat; } | $BIN "$@" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tr '\r' '\n'; }
md5() { md5sum < "$1" | awk '{print $1}'; }

[ -x "$BIN" ] || { echo "No existe $BIN. Ejecuta 'make' primero."; exit 1; }

# Archivo de texto grande (~20 MB) para que la compresion tarde lo suficiente.
for i in $(seq 400); do cat ../huffman/*.c editor_*.c; done > "$DIR/grande.txt"
cp "$DIR/grande.txt" "$DIR/original.txt"
MD5_ORIG=$(md5 "$DIR/original.txt")

titulo "1-2. Segundo plano + edicion concurrente rechazada"
SALIDA=$(printf 'z %s 2\na intento de edicion\nd 1\nj\nw\n' "$DIR/grande.huf" | editor "$DIR/grande.txt")
contiene "el prompt vuelve mientras comprime (etiqueta de progreso)" "$SALIDA" "\[z [0-9]*%\]"
contiene "la edicion 'a' se rechaza mientras se lee la fuente" "$SALIDA" "Archivo en uso"
contiene "la compresion termina y se informa"  "$SALIDA" "Compresion terminada"
comprobar "el archivo fuente NO fue modificado" "$(md5 "$DIR/grande.txt")" "$MD5_ORIG"
[ -f "$DIR/grande.huf" ] && comprobar "se creo el .huf" "si" "si" || comprobar "se creo el .huf" "no" "si"

titulo "3. Integridad round-trip desde el editor"
SALIDA=$(printf 'u %s %s 4\nw\n' "$DIR/grande.huf" "$DIR/recuperado.txt" | editor)
contiene "la descompresion termina" "$SALIDA" "Descompresion terminada"
comprobar "md5(recuperado) == md5(original)" "$(md5 "$DIR/recuperado.txt")" "$MD5_ORIG"

titulo "4. Edicion permitida cuando no hay tarea"
SALIDA=$(printf 'a linea despues\n' | editor "$DIR/grande.txt")
contiene "a funciona sin tarea activa" "$SALIDA" "anexada al final"

titulo "5. Cancelacion con k"
cp "$DIR/original.txt" "$DIR/grande.txt"
SALIDA=$(printf 'z %s 2\nk\nw\n' "$DIR/cancelado.huf" | editor "$DIR/grande.txt")
contiene "se informa la cancelacion" "$SALIDA" "cancelada"
[ -e "$DIR/cancelado.huf" ] && comprobar "no queda salida parcial" "existe" "no existe" \
                            || comprobar "no queda salida parcial" "no existe" "no existe"

titulo "6. Validaciones"
SALIDA=$(printf 'z %s\n' "$DIR/grande.txt" | editor "$DIR/grande.txt")
contiene "salida == archivo abierto se rechaza" "$SALIDA" "no pueden ser el mismo"
SALIDA=$(printf 'u %s %s\n' "$DIR/no_existe.huf" "$DIR/x.txt" | editor)
contiene "entrada inexistente se reporta" "$SALIDA" "No such file"
printf 'basura' > "$DIR/falso.huf"
SALIDA=$(printf 'u %s %s\nw\n' "$DIR/falso.huf" "$DIR/x.txt" | editor)
contiene ".huf invalido se rechaza" "$SALIDA" "fallo"
[ -e "$DIR/x.txt" ] && comprobar "no queda salida de un .huf invalido" "existe" "no existe" \
                    || comprobar "no queda salida de un .huf invalido" "no existe" "no existe"

echo
echo "  Pruebas exitosas: $OK"
echo "  Pruebas fallidas: $FALLOS"
rm -rf "$DIR"
exit $FALLOS
