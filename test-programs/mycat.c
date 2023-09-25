#include <stdio.h>

int main(int argc, char** argv) {
    FILE* file = stdin;
    if (argc >= 2) {
        file = fopen(argv[1], "r");
        if (!file) {
            fprintf(stderr, "Error: couldn't open %s\n", argv[1]);
            return 1;
        }
    }
    printf("mycat: ");
    while (1) {
        int ch = fgetc(file);
        if (ch == EOF) {
            break;
        }
        putchar(ch);
        if (ch == '\n')
            printf("mycat: ");
    }
}
