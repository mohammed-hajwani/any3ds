/* M6: the canonical 3DS homebrew "hello world", built against the real
 * libctru (and newlib). Recompiling the whole linked program shows what
 * real 3DS library code needs; running it needs the OS layer (svc + IPC
 * services) that comes next. */
#include <3ds.h>
#include <stdio.h>

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("Hello from recompiled code!\n");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
