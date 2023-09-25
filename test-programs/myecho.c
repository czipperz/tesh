#include <stdio.h>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i >= 2)
            putchar(' ');
        fputs(argv[i], stdout);
    }
    putchar('\n');
}
