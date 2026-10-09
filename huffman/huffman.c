/* ============================================================================
 *  huffman.c  -  Construccion del arbol de Huffman y generacion de codigos
 * ----------------------------------------------------------------------------
 *  Esta parte es la ALGORITMICA (secuencial): se ejecuta UNA sola vez despues
 *  de que el conteo de frecuencias concurrente termino. Construye el arbol
 *  global y, a partir de el, la tabla de codigos usada por todos los workers
 *  de compresion.
 *
 *  Se usa un min-heap (cola de prioridad) propio para no depender de librerias
 *  de alto nivel, como exige el enunciado del parcial.
 * ==========================================================================*/
#include "huffman.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Min-heap de punteros a HufNode, ordenado por frecuencia           */
/* ------------------------------------------------------------------ */
typedef struct {
    HufNode **data;
    int       size;
    int       cap;
} MinHeap;

static MinHeap *heap_create(int cap) {
    MinHeap *h = malloc(sizeof(MinHeap));
    if (!h) return NULL;
    h->data = malloc(sizeof(HufNode *) * cap);
    if (!h->data) { free(h); return NULL; }
    h->size = 0;
    h->cap  = cap;
    return h;
}

static void heap_swap(HufNode **a, HufNode **b) {
    HufNode *t = *a; *a = *b; *b = t;
}

static void heap_push(MinHeap *h, HufNode *n) {
    int i = h->size++;
    h->data[i] = n;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h->data[parent]->freq <= h->data[i]->freq) break;
        heap_swap(&h->data[parent], &h->data[i]);
        i = parent;
    }
}

static HufNode *heap_pop(MinHeap *h) {
    if (h->size == 0) return NULL;
    HufNode *top = h->data[0];
    h->data[0] = h->data[--h->size];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, smallest = i;
        if (l < h->size && h->data[l]->freq < h->data[smallest]->freq) smallest = l;
        if (r < h->size && h->data[r]->freq < h->data[smallest]->freq) smallest = r;
        if (smallest == i) break;
        heap_swap(&h->data[i], &h->data[smallest]);
        i = smallest;
    }
    return top;
}

static void heap_destroy(MinHeap *h) {
    if (h) { free(h->data); free(h); }
}

/* ------------------------------------------------------------------ */
/*  Creacion de nodos                                                 */
/* ------------------------------------------------------------------ */
static HufNode *node_new(int32_t symbol, uint64_t freq, HufNode *l, HufNode *r) {
    HufNode *n = malloc(sizeof(HufNode));
    if (!n) return NULL;
    n->symbol = symbol;
    n->freq   = freq;
    n->left   = l;
    n->right  = r;
    return n;
}

/* ------------------------------------------------------------------ */
/*  huf_build_tree: construye el arbol de Huffman a partir de las     */
/*  frecuencias globales ya acumuladas.                               */
/* ------------------------------------------------------------------ */
HufNode *huf_build_tree(const uint64_t freq[HUF_NSYM]) {
    MinHeap *h = heap_create(HUF_NSYM + 1);
    if (!h) return NULL;

    int distintos = 0;
    for (int s = 0; s < HUF_NSYM; s++) {
        if (freq[s] > 0) {
            HufNode *leaf = node_new(s, freq[s], NULL, NULL);
            if (!leaf) { heap_destroy(h); return NULL; }
            heap_push(h, leaf);
            distintos++;
        }
    }

    /* Caso borde: archivo vacio -> no hay arbol */
    if (distintos == 0) { heap_destroy(h); return NULL; }

    /* Caso borde: un unico simbolo -> arbol de un solo nivel para que
       el codigo tenga longitud 1 (no 0 bits). */
    if (distintos == 1) {
        HufNode *only = heap_pop(h);
        HufNode *root = node_new(-1, only->freq, only, NULL);
        heap_destroy(h);
        return root;
    }

    while (h->size > 1) {
        HufNode *a = heap_pop(h);
        HufNode *b = heap_pop(h);
        HufNode *parent = node_new(-1, a->freq + b->freq, a, b);
        if (!parent) { heap_destroy(h); return NULL; }
        heap_push(h, parent);
    }

    HufNode *root = heap_pop(h);
    heap_destroy(h);
    return root;
}

void huf_free_tree(HufNode *root) {
    if (!root) return;
    huf_free_tree(root->left);
    huf_free_tree(root->right);
    free(root);
}

/* ------------------------------------------------------------------ */
/*  Generacion de codigos: DFS acumulando bits.                       */
/* ------------------------------------------------------------------ */
static void set_bit(uint8_t *bits, int pos, int value) {
    if (value)
        bits[pos / 8] |=  (uint8_t)(1u << (7 - (pos % 8)));
    else
        bits[pos / 8] &= (uint8_t)~(1u << (7 - (pos % 8)));
}

static void codes_dfs(const HufNode *node, HufCode *acc, HufCode codes[HUF_NSYM]) {
    if (!node) return;

    if (node->symbol >= 0) {               /* hoja */
        if (acc->length == 0) {            /* arbol de un solo simbolo */
            codes[node->symbol].length  = 1;
            memset(codes[node->symbol].bits, 0, sizeof(codes[node->symbol].bits));
        } else {
            codes[node->symbol] = *acc;
        }
        return;
    }

    /* izquierda = 0 */
    HufCode left = *acc;
    if (left.length < HUF_MAX_CODE_LEN) {
        set_bit(left.bits, left.length, 0);
        left.length++;
        codes_dfs(node->left, &left, codes);
    }

    /* derecha = 1 */
    HufCode right = *acc;
    if (right.length < HUF_MAX_CODE_LEN) {
        set_bit(right.bits, right.length, 1);
        right.length++;
        codes_dfs(node->right, &right, codes);
    }
}

void huf_build_codes(const HufNode *root, HufCode codes[HUF_NSYM]) {
    memset(codes, 0, sizeof(HufCode) * HUF_NSYM);
    if (!root) return;
    HufCode acc;
    memset(&acc, 0, sizeof(acc));
    acc.length = 0;
    codes_dfs(root, &acc, codes);
}
