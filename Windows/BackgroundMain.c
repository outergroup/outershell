#include <windows.h>
#include <shellapi.h>

int wmain(int argc, wchar_t **argv);

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    (void)instance;
    (void)previous;
    (void)command;
    (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    int result = wmain(argc, argv);
    LocalFree(argv);
    return result;
}
