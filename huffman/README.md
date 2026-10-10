# Compresor de Archivos Huffman Concurrente (Parcial 2 - SO)

**Alternativa 1:** Compresor de Archivos Huffman Concurrente Integrado al Editor.

Modulo de compresion/descompresion basado en **Codificacion de Huffman** con
**procesamiento concurrente** mediante hilos POSIX (`pthread`). Se usa de dos formas:

1. **Integrado al editor del Parcial 1** (`shell/edi` y `edi` dentro de `eafitOS`):
   comandos `z`, `u`, `j`, `w`, `k` que comprimen/descomprimen en un **hilo de fondo**
   sin congelar el editor (ver `shell/editor_huf.c`).
2. **Binario independiente** `huffman` para pruebas y medicion de tiempos.

---

## Arquitectura y archivos

| Archivo                  | Responsabilidad                                                          |
|--------------------------|--------------------------------------------------------------------------|
| `huffman.h`              | API publica, estructuras y formato del archivo `.huf`.                   |
| `huffman.c`              | Arbol de Huffman (min-heap propio) y generacion de codigos. *(secuencial)* |
| `freq_parallel.c`        | **Conteo de frecuencias concurrente**: reduccion local + mutex al combinar. |
| `worker_pool.c`          | **Pool de hilos** + cola de tareas sincronizada (mutex + 2 cond).        |
| `compress.c`             | **Compresion por bloques en paralelo** + coordinador que escribe en orden. |
| `decompress.c`           | **Descompresion por bloques en paralelo** + escritura en orden.          |
| `progress.c`             | Progreso compartido (mutex + cond `cambio`), barra en tiempo real.       |
| `huf_io.c`               | E/S robusta: reintenta `EINTR` y transferencias parciales.               |
| `main.c`                 | CLI del compresor + manejo de `SIGINT`/`SIGTERM`.                        |
| `test_huffman.sh`        | Integridad round-trip (md5), `.huf` corrupto y cancelacion por senal.    |
| `../shell/editor_huf.c`  | **Integracion con el editor**: hilo de fondo, progreso y `pthread_rwlock`. |
| `../shell/pruebas_huffman_editor.sh` | Pruebas de la integracion (segundo plano, carreras, cancelacion). |

---

## Como mapea a la rubrica (100 pts)

- **Concurrencia en Huffman (25):** el conteo de frecuencias reparte el archivo entre
  N hilos (reduccion local por hilo) y la codificacion de cada bloque de 64 KB la hace
  el pool de hilos. Un hilo coordinador escribe los bloques en orden secuencial.
- **Mecanismos de Sincronizacion (25):** `pthread_mutex` + `pthread_cond` en el pool,
  en el coordinador (`done[i]`), en el progreso y en el handshake del editor;
  `pthread_rwlock` (lectores/escritores) entre el compresor y la edicion. **Sin
  busy-waiting**: toda espera es `pthread_cond_wait` (tambien la barra de progreso),
  siempre dentro de un `while`. Sin deadlocks: nunca se toman dos bloqueos a la vez y
  los workers **siempre** marcan su bloque como terminado, incluso con error.
- **Integracion con el Editor (20):** `z`/`u` lanzan un hilo de fondo y el prompt
  vuelve de inmediato mostrando el porcentaje (`edi:nota.txt [z 45%]>`); `j` muestra
  la barra, `w` la sigue en vivo, `k` cancela. Mientras el compresor lee la fuente
  tiene un `rdlock`; `a`, `i`, `d`, `x` piden el `wrlock` con `trywrlock` y se
  **rechazan** sin bloquear la interfaz. El archivo de salida tampoco se puede editar
  mientras se escribe.
- **Correccion Algoritmica (15):** la tabla de frecuencias va en la cabecera para
  reconstruir el mismo arbol; `make test` y `make pruebas-huf` comparan md5.
  Ademas el compresor verifica que el archivo no cambio entre el conteo y la lectura.
- **Manejo de Errores y Recursos (15):** `pthread_join` de todos los hilos (tambien
  el de fondo del editor), cierre de descriptores, liberacion de memoria, validacion
  de `malloc`, `.huf` corrupto rechazado, salida parcial borrada ante error o
  cancelacion, y `SIGINT`/`SIGTERM` manejados con una bandera `sig_atomic_t`.
  Verificado con AddressSanitizer y ThreadSanitizer sin reportes.

---

## Compilacion y uso

```bash
# --- Binario independiente ---
cd huffman
make                                           # compila ./huffman
./huffman compress   archivo.txt archivo.huf 4  # n_hilos opcional
./huffman decompress archivo.huf recuperado.txt 4
make test                                      # pruebas de integridad
make clean

# --- Integrado al editor (Parcial 1) ---
cd ../shell
make                    # compila edi y eafitOS (incluye el modulo Huffman)
make pruebas            # 50 pruebas del editor (Parcial 1)
make pruebas-huf        # pruebas de la integracion (Parcial 2)
./edi notas.txt
```

Dentro del editor:

| Comando                   | Accion                                              |
|---------------------------|-----------------------------------------------------|
| `z [salida.huf] [hilos]`  | Comprime el archivo abierto en segundo plano.       |
| `u <ent.huf> <sal> [h]`   | Descomprime en segundo plano.                       |
| `j`                       | Estado y porcentaje de la tarea.                    |
| `w`                       | Barra de progreso en vivo hasta que termine.        |
| `k`                       | Cancela la tarea (borra la salida parcial).         |

Si los scripts fallan con `$'\r': command not found` (clonado en Windows), ejecutar
`sed -i 's/\r$//' *.sh`.

---

## Formato del archivo `.huf`

```
[MAGIC "HUF1" 4B]
[uint64 tamano_original]
[uint32 num_simbolos]
[ (uint8 simbolo, uint64 frecuencia) * num_simbolos ]   -> tabla de frecuencias
[uint32 num_bloques]
[ por bloque y en orden: (uint64 bits_validos, uint64 bytes, datos...) ]
```

Guardar la tabla de frecuencias permite reconstruir exactamente el mismo arbol de
Huffman al descomprimir, garantizando la integridad del contenido.
