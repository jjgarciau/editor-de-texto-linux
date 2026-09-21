#!/usr/bin/env bash
# ====================================================================================
#  pruebas_editor.sh  -  SCRIPT DE PRUEBAS AUTOMATIZADAS DEL EDITOR (SO2026B)
# ====================================================================================
#  Ejercita los 10 comandos del editor y valida el CONTENIDO REAL del archivo en disco
#  despues de cada operacion, no solo la salida por pantalla. Incluye una bateria de
#  casos borde para demostrar la robustez exigida por la rubrica.
#
#  Uso:   bash pruebas_editor.sh      (o bien:  make pruebas)
# ====================================================================================

set -u
BIN="./edi"
DIR_TRABAJO="$(mktemp -d /tmp/pruebas_edi.XXXXXX)"
OK=0
FALLOS=0

VERDE="\033[1;32m"; ROJO="\033[1;31m"; AZUL="\033[1;36m"; GRIS="\033[0;90m"; RESET="\033[0m"

titulo() { echo -e "\n${AZUL}===== $1 =====${RESET}"; }

# comprobar <descripcion> <valor_obtenido> <valor_esperado>
comprobar() {
    local desc="$1" obtenido="$2" esperado="$3"
    if [ "$obtenido" == "$esperado" ]; then
        echo -e "  ${VERDE}[OK]${RESET}  $desc"
        OK=$((OK+1))
    else
        echo -e "  ${ROJO}[FALLA]${RESET} $desc"
        echo -e "     ${GRIS}esperado : $(printf '%q' "$esperado")${RESET}"
        echo -e "     ${GRIS}obtenido : $(printf '%q' "$obtenido")${RESET}"
        FALLOS=$((FALLOS+1))
    fi
}

# contiene <descripcion> <texto> <patron>
contiene() {
    local desc="$1" texto="$2" patron="$3"
    if echo "$texto" | grep -q -- "$patron"; then
        echo -e "  ${VERDE}[OK]${RESET}  $desc"
        OK=$((OK+1))
    else
        echo -e "  ${ROJO}[FALLA]${RESET} $desc (no se encontro '$patron')"
        FALLOS=$((FALLOS+1))
    fi
}

# ejecuta el editor alimentando comandos por STDIN; 't' desactiva el trazado
editor() {
    { echo "t"; cat; } | $BIN 2>&1
}

if [ ! -x "$BIN" ]; then
    echo "No existe el binario $BIN. Ejecuta 'make' primero."
    exit 1
fi

echo -e "${AZUL}Directorio temporal de pruebas: $DIR_TRABAJO${RESET}"

# ------------------------------------------------------------------------------------
titulo "1. Comando 'o' + 'a' : creacion de archivo y anexado (open, lseek SEEK_END, write)"
# ------------------------------------------------------------------------------------
F="$DIR_TRABAJO/t1.txt"
editor > /dev/null <<EOF
o $F
a primera linea
a segunda linea
a tercera linea
q
EOF
comprobar "El archivo se creo con 3 lineas" "$(wc -l < "$F")" "3"
comprobar "Contenido correcto" "$(cat "$F")" "$(printf 'primera linea\nsegunda linea\ntercera linea')"
comprobar "Permisos 0644 al crear" "$(stat -c '%a' "$F")" "644"

# ------------------------------------------------------------------------------------
titulo "2. Comando 'p' : impresion total y por numero de linea (lseek + read)"
# ------------------------------------------------------------------------------------
SALIDA=$(editor <<EOF
o $F
p
p 2
q
EOF
)
contiene "'p' numera las lineas"          "$SALIDA" "| primera linea"
contiene "'p 2' imprime solo la linea 2"  "$SALIDA" "| segunda linea"
contiene "Reporta el tamano del archivo"  "$SALIDA" "bytes"

# ------------------------------------------------------------------------------------
titulo "3. Comando 'i' : insercion arbitraria con desplazamiento de bytes"
# ------------------------------------------------------------------------------------
editor > /dev/null <<EOF
o $F
i 2 linea intercalada
q
EOF
comprobar "La insercion desplazo el resto sin perder datos" "$(cat "$F")" \
    "$(printf 'primera linea\nlinea intercalada\nsegunda linea\ntercera linea')"

editor > /dev/null <<EOF
o $F
i 1 encabezado
q
EOF
comprobar "Insercion en la primera linea" "$(head -1 "$F")" "encabezado"

editor > /dev/null <<EOF
o $F
i 6 pie de archivo
q
EOF
comprobar "Insercion en n+1 equivale a anexar" "$(tail -1 "$F")" "pie de archivo"

# ------------------------------------------------------------------------------------
titulo "4. Comando 'd' : borrado con compactacion y ftruncate"
# ------------------------------------------------------------------------------------
TAM_ANTES=$(stat -c '%s' "$F")
editor > /dev/null <<EOF
o $F
d 1
q
EOF
TAM_DESPUES=$(stat -c '%s' "$F")
comprobar "La primera linea fue eliminada" "$(head -1 "$F")" "primera linea"
comprobar "ftruncate redujo el tamano en 11 bytes ('encabezado'+\\n)" \
    "$((TAM_ANTES - TAM_DESPUES))" "11"

editor > /dev/null <<EOF
o $F
d 5
q
EOF
comprobar "Borrado de la ultima linea" "$(tail -1 "$F")" "tercera linea"
comprobar "No quedan bytes fantasma al final" "$(tail -c 1 "$F" | xxd -p)" "0a"

# ------------------------------------------------------------------------------------
titulo "5. Comando 's' : busqueda simple con linea y columna"
# ------------------------------------------------------------------------------------
SALIDA=$(editor <<EOF
o $F
s linea
q
EOF
)
contiene "Encuentra coincidencias"           "$SALIDA" "coincidencia(s) encontradas"
contiene "Reporta numero de linea y columna" "$SALIDA" "columna"
SALIDA=$(editor <<EOF
o $F
s zzzz_inexistente
q
EOF
)
contiene "Informa cuando no hay coincidencias" "$SALIDA" "Sin coincidencias"

# ------------------------------------------------------------------------------------
titulo "6. Comando 'm' : metadatos del inodo via fstat(2)"
# ------------------------------------------------------------------------------------
INODO_REAL=$(stat -c '%i' "$F")
SALIDA=$(editor <<EOF
o $F
m
q
EOF
)
contiene "Muestra el numero de inodo real"  "$SALIDA" "$INODO_REAL"
contiene "Muestra los permisos en formato ls" "$SALIDA" "rw-"
contiene "Muestra la fecha de modificacion" "$SALIDA" "Modificacion:"
contiene "Muestra el tamano en bytes"       "$SALIDA" "$(stat -c '%s' "$F") bytes"

# ------------------------------------------------------------------------------------
titulo "7. Comandos 'y' / 'x' : portapapeles secuencial (FIFO)"
# ------------------------------------------------------------------------------------
G="$DIR_TRABAJO/t7.txt"
editor > /dev/null <<EOF
o $G
a alfa
a beta
a gamma
y 1
y 3
x 2
q
EOF
comprobar "El primer 'x' pega la copia mas antigua (FIFO)" "$(cat "$G")" \
    "$(printf 'alfa\nalfa\nbeta\ngamma')"

editor > /dev/null <<EOF
o $G
y 1
y 4
x
q
EOF
comprobar "'x' sin argumento pega al final" "$(tail -1 "$G")" "alfa"

SALIDA=$(editor <<EOF
o $G
y 2
c
q
EOF
)
contiene "'c' lista el portapapeles pendiente" "$SALIDA" "Portapapeles secuencial"

# ------------------------------------------------------------------------------------
titulo "8. CASOS BORDE"
# ------------------------------------------------------------------------------------
# 8.1 Archivo inexistente en un directorio inexistente -> perror(open)
SALIDA=$(editor <<EOF
o /directorio_que_no_existe_1234/x.txt
q
EOF
)
contiene "open() fallido reportado con perror" "$SALIDA" "open"
contiene "Mensaje del errno del sistema"       "$SALIDA" "No such file or directory"

# 8.2 Archivo sin permiso de escritura -> EACCES
H="$DIR_TRABAJO/solo_lectura.txt"
echo "contenido" > "$H"; chmod 444 "$H"
SALIDA=$(editor <<EOF
o $H
q
EOF
)
contiene "Permiso denegado al abrir en O_RDWR" "$SALIDA" "Permission denied"
chmod 644 "$H"

# 8.3 Numero de linea fuera de rango
SALIDA=$(editor <<EOF
o $F
p 99
d 0
y 99
q
EOF
)
contiene "Rechaza linea superior al total" "$SALIDA" "fuera de rango"
contiene "Rechaza linea 0 (indices 1-based)" "$SALIDA" "fuera de rango"

# 8.4 Operar sin archivo abierto
SALIDA=$(editor <<EOF
p
a texto
q
EOF
)
contiene "Exige abrir un archivo primero" "$SALIDA" "No hay archivo abierto"

# 8.5 Portapapeles vacio
SALIDA=$(editor <<EOF
o $F
x 1
q
EOF
)
contiene "Rechaza pegar con portapapeles vacio" "$SALIDA" "portapapeles esta vacio"

# 8.6 Archivo vacio
V="$DIR_TRABAJO/vacio.txt"
: > "$V"
SALIDA=$(editor <<EOF
o $V
p
d 1
q
EOF
)
contiene "Detecta archivo vacio en 'p'" "$SALIDA" "(archivo vacio)"
contiene "Detecta archivo vacio en 'd'" "$SALIDA" "El archivo esta vacio"

# 8.7 Archivo que NO termina en salto de linea
W="$DIR_TRABAJO/sin_salto.txt"
printf 'ultima sin salto' > "$W"
editor > /dev/null <<EOF
o $W
a nueva linea
q
EOF
comprobar "Anexa insertando el '\\n' faltante" "$(cat "$W")" \
    "$(printf 'ultima sin salto\nnueva linea')"

# 8.8 Comando inexistente
SALIDA=$(editor <<EOF
o $F
zz
pp
q
EOF
)
contiene "Rechaza comandos desconocidos" "$SALIDA" "no reconocido"

# 8.9 Linea muy larga (prueba del buffer dinamico)
L="$DIR_TRABAJO/larga.txt"
TEXTO_LARGO=$(head -c 3000 /dev/zero | tr '\0' 'X')
editor > /dev/null <<EOF
o $L
a $TEXTO_LARGO
a corta
i 1 $TEXTO_LARGO
q
EOF
comprobar "Maneja lineas de 3000 bytes sin truncar" "$(head -1 "$L" | wc -c)" "3001"
comprobar "El archivo mantiene 3 lineas" "$(wc -l < "$L")" "3"

# 8.10 Ctrl+D (EOF) equivale a 'q' y cierra el descriptor
SALIDA=$(printf 'o %s\np 1\n' "$F" | $BIN 2>&1)
contiene "EOF cierra el editor limpiamente" "$SALIDA" "Editor cerrado"
contiene "EOF ejecuta close(2)"             "$SALIDA" "close"

# ------------------------------------------------------------------------------------
titulo "9. Verificacion de la restriccion de E/S del enunciado"
# ------------------------------------------------------------------------------------
PROHIBIDAS=$(grep -nE '\b(fopen|fread|fwrite|fclose|fprintf\(f|fseek|rewind)\b' \
    editor_core.c editor_edit.c editor_meta.c cat_edicion.c edi_main.c || true)
comprobar "Sin funciones de E/S de alto nivel sobre el archivo" "$PROHIBIDAS" ""

for llamada in open read write lseek ftruncate fstat close; do
    contiene "Se utiliza la syscall $llamada()" \
        "$(cat editor_core.c editor_edit.c editor_meta.c)" "$llamada("
done

# ------------------------------------------------------------------------------------
titulo "10. Integracion con el shell EAFITOS"
# ------------------------------------------------------------------------------------
if [ -x ./eafitOS ]; then
    SALIDA=$(printf 'help\nhelp edicion\nedi %s\np 1\nq\nexit\n' "$F" | ./eafitOS 2>&1)
    contiene "La categoria 'edicion' aparece en 'help'"      "$SALIDA" "edicion"
    contiene "'help edicion' documenta el comando edi"       "$SALIDA" "edi"
    contiene "El shell lanza el editor y este abre el archivo" "$SALIDA" "abierto (fd="
    contiene "Al salir del editor se retorna al shell"        "$SALIDA" "Regresando al shell"
else
    echo "  (eafitOS no compilado: se omite la prueba de integracion)"
fi

# ------------------------------------------------------------------------------------
echo -e "\n${AZUL}==================== RESUMEN ====================${RESET}"
echo -e "  Pruebas exitosas: ${VERDE}$OK${RESET}"
echo -e "  Pruebas fallidas: ${ROJO}$FALLOS${RESET}"
rm -rf "$DIR_TRABAJO"
if [ "$FALLOS" -eq 0 ]; then
    echo -e "  ${VERDE}TODAS LAS PRUEBAS PASARON${RESET}\n"
    exit 0
else
    echo -e "  ${ROJO}HAY PRUEBAS FALLIDAS${RESET}\n"
    exit 1
fi
