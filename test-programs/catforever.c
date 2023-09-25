#include <stdio.h>

int main() {
    while (1) {
        int ch = getchar();
        if (ch == EOF) {
            printf("EOF\n");
            continue;
        }
        putchar(ch);
    }
}
