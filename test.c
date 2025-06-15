#include <windows.h>
#include <stdio.h>

void logMessage(const char *msg) {
    OutputDebugStringA(msg);
}

int main() {
    logMessage("Hello, Debug Console!\n");
    return 0;
}
