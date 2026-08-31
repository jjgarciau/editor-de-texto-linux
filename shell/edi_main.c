
#include "editor.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "Uso: %s [archivo]\n", argv[0]);
        return 1;
    }
    return ed_repl(argc == 2 ? argv[1] : NULL);
}
