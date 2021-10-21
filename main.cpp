int actual_main(int argc, char** argv);

#if defined(_WIN32) && !defined(CONSOLE_MAIN)
#include <windows.h>
int WINAPI WinMain(_In_ HINSTANCE hInstance,
                   _In_opt_ HINSTANCE hPrevInstance,
                   _In_ LPSTR lpCmdLine,
                   _In_ int nShowCmd) {
    return actual_main(__argc, __argv);
}
#else
int main(int argc, char** argv) {
    return actual_main(argc, argv);
}
#endif
