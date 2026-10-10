#!/usr/bin/env bash
# ============================================================================
#  test_huffman.sh  -  Prueba de integridad del compresor Huffman concurrente
#  Verifica que descomprimir(comprimir(X)) == X  (criterio: Correccion
#  Algoritmica, 15 pts). Usa md5sum/diff.
# ============================================================================
set -u
BIN=./huffman
TMP=$(mktemp -d)
FALLOS=0

echo "== Prueba de integridad Huffman concurrente =="
echo "Directorio temporal: $TMP"

probar() {
    local nombre="$1"
    local archivo="$2"
    local hilos="$3"

    "$BIN" compress   "$archivo"       "$TMP/c.huf" "$hilos"  2>/dev/null
    "$BIN" decompress "$TMP/c.huf"     "$TMP/d.out" "$hilos"  2>/dev/null

    local a b
    a=$(md5sum < "$archivo" | awk '{print $1}')
    b=$(md5sum < "$TMP/d.out" | awk '{print $1}')

    local orig comp
    orig=$(wc -c < "$archivo")
    comp=$(wc -c < "$TMP/c.huf")

    if [ "$a" == "$b" ]; then
        printf "  [OK]   %-22s hilos=%s  %s B -> %s B (md5 coincide)\n" \
               "$nombre" "$hilos" "$orig" "$comp"
    else
        printf "  [FALLO]%-22s hilos=%s  md5 NO coincide!\n" "$nombre" "$hilos"
        FALLOS=$((FALLOS+1))
    fi
}

# 1) Archivo vacio
: > "$TMP/vacio.bin"
probar "vacio" "$TMP/vacio.bin" 4

# 2) Un solo caracter repetido (caso borde: arbol de un simbolo)
head -c 5000 /dev/zero | tr '\0' 'A' > "$TMP/un_simbolo.txt"
probar "un_simbolo" "$TMP/un_simbolo.txt" 4

# 3) Texto pequeno
printf 'Hola Sistemas Operativos 2026 - Huffman concurrente!\n' > "$TMP/texto.txt"
probar "texto_pequeno" "$TMP/texto.txt" 1
probar "texto_pequeno" "$TMP/texto.txt" 8

# 4) Datos binarios aleatorios medianos (varios bloques)
head -c 1048576 /dev/urandom > "$TMP/rand1mb.bin"
probar "aleatorio_1MB" "$TMP/rand1mb.bin" 4
probar "aleatorio_1MB" "$TMP/rand1mb.bin" 8

# 5) Archivo grande para ver concurrencia (5 MB)
head -c 5242880 /dev/urandom > "$TMP/rand5mb.bin"
probar "aleatorio_5MB" "$TMP/rand5mb.bin" 8

# 6) El propio codigo fuente (texto real con patrones)
cat ./*.c > "$TMP/fuentes.txt" 2>/dev/null
probar "codigo_fuente" "$TMP/fuentes.txt" 4

# 7) Archivo .huf corrupto / truncado: debe fallar SIN dejar salida.
"$BIN" compress "$TMP/fuentes.txt" "$TMP/ok.huf" 4 2>/dev/null
head -c 200 "$TMP/ok.huf" > "$TMP/truncado.huf"
"$BIN" decompress "$TMP/truncado.huf" "$TMP/trunc.out" 4 2>/dev/null
if [ $? -ne 0 ] && [ ! -e "$TMP/trunc.out" ]; then
    printf "  [OK]   %-22s se rechaza y no deja salida parcial\n" "huf_truncado"
else
    printf "  [FALLO]%-22s no se detecto la corrupcion\n" "huf_truncado"; FALLOS=$((FALLOS+1))
fi

# 8) Senal SIGINT (Ctrl+C) a mitad de la compresion: cancelacion limpia.
for i in $(seq 40); do cat "$TMP/rand5mb.bin"; done > "$TMP/grande.bin"   # 200 MB
"$BIN" compress "$TMP/grande.bin" "$TMP/grande.huf" 2 2>/dev/null &
PID=$!
sleep 0.3
kill -INT "$PID" 2>/dev/null
wait "$PID"; RC=$?
if [ "$RC" -eq 130 ] && [ ! -e "$TMP/grande.huf" ]; then
    printf "  [OK]   %-22s SIGINT -> codigo 130, sin salida parcial\n" "senal_sigint"
else
    printf "  [FALLO]%-22s rc=%s\n" "senal_sigint" "$RC"; FALLOS=$((FALLOS+1))
fi

echo "---------------------------------------------"
if [ "$FALLOS" -eq 0 ]; then
    echo "RESULTADO: TODAS las pruebas pasaron. Integridad garantizada."
else
    echo "RESULTADO: $FALLOS prueba(s) FALLARON."
fi

rm -rf "$TMP"
exit $FALLOS
