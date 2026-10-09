# Compresor de Archivos Huffman Concurrente (Parcial 2 - SO)

**Alternativa 1:** Compresor de Archivos Huffman Concurrente Integrado al Editor.

Módulo de compresión/descompresión basado en **Codificación de Huffman** con
**procesamiento concurrente** mediante hilos POSIX (`pthread`). Es un módulo
**independiente** del editor del Parcial 1 (carpeta `shell/`): se compila y
ejecuta por separado, y puede invocarse desde la CLI del editor como un proceso
en segundo plano sin modificar su código.

---

## Arquitectura y archivos

| Archivo            | Responsabilidad                                                        |
|--------------------|------------------------------------------------------------------------|
| `huffman.h`        | API pública, estructuras y formato del archivo `.huf`.                 |
| `huffman.c`        | Árbol de Huffman (min-heap propio) y generación de códigos. *(secuencial)* |
| `freq_parallel.c`  | **Conteo de frecuencias concurrente** con reducción local por hilo.    |
| `worker_pool.c`    | **Pool de hilos** + cola de tareas sincronizada (mutex + cond).        |
| `progress.c`       | Barra de progreso en tiempo real (hilo impresor).                      |
| `compress.c`       | **Compresión por bloques en paralelo** + ensamblado ordenado.          |
| `decompress.c`     | **Descompresión por bloques en paralelo** + ensamblado ordenado.       |
| `main.c`           | CLI del compresor.                                                     |
| `test_huffman.sh`  | Prueba de integridad round-trip (md5).                                 |

---

## Cómo mapea a la rúbrica (100 pts)

- **Concurrencia en Huffman (25):** conteo de frecuencias en paralelo (cada
  hilo lee una región del archivo y reduce localmente) + compresión de cada
  bloque de 64 KB en paralelo por el pool de hilos. La salida se ensambla en
  orden secuencial correcto.
- **Mecanismos de Sincronización (25):** `pthread_mutex` + `pthread_cond`
  en el pool de tareas y en el coordinador de escritura. **Sin busy-waiting**
  (siempre `pthread_cond_wait`), sin condiciones de carrera (regiones críticas
  mínimas), sin deadlocks (orden de bloqueo consistente).
- **Integración con el Editor (20):** binario independiente que se lanza en
  background (`./huffman ... &`) sin congelar la UI; barra de progreso en
  tiempo real por `stderr`.
- **Corrección Algorítmica (15):** la tabla de frecuencias se guarda en la
  cabecera para reconstruir el mismo árbol; `test_huffman.sh` verifica con
  `md5sum` que el descomprimido es idéntico al original.
- **Manejo de Errores y Recursos (15):** `pthread_join` de todos los hilos,
  cierre de descriptores, liberación de toda la memoria dinámica. Verificado
  con AddressSanitizer/LeakSanitizer sin errores.

---

## Compilación y uso

```bash
# Compilar (requiere gcc + make en Linux/POSIX)
make

# Comprimir  (n_hilos opcional; por defecto = núcleos del sistema)
./huffman compress   archivo.txt   archivo.huf   4

# Descomprimir
./huffman decompress archivo.huf   archivo_recuperado.txt   4

# Prueba de integridad completa (round-trip + md5)
make test

# Limpiar objetos y binario
make clean
```

### Integración en segundo plano con el editor

El editor (o cualquier shell) puede invocar el compresor como proceso aparte
para no bloquear la interfaz:

```bash
./huffman compress nota.txt nota.huf 4 &   # '&' -> background
```

Esto cumple el requisito de ejecución asíncrona sin modificar el código del
editor en `shell/`.

---

## Formato del archivo `.huf`

```
[MAGIC "HUF1" 4B]
[uint64 tamaño_original]
[uint32 num_símbolos]
[ (uint8 símbolo, uint64 frecuencia) * num_símbolos ]   -> tabla de frecuencias
[uint32 num_bloques]
[ por bloque y en orden: (uint64 bits_válidos, uint64 bytes, datos...) ]
```

Guardar la tabla de frecuencias permite reconstruir exactamente el mismo árbol
de Huffman al descomprimir, garantizando la integridad del contenido.
