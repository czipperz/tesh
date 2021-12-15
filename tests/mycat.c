#include <stdio.h>

int main() {
    printf("mycat: ");
    while (1) {
        int ch = getchar();
        if (ch == EOF) {
            break;
        }
        putchar(ch);
        if (ch == '\n')
            printf("mycat: ");
    }
}
