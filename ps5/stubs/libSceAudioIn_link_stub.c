/* (ps5, lot 86) The link's stand-in for the console's libSceAudioIn: the five calls host/posix/hos_sdl_ps5.c makes for player 1's
   DualSense microphone (host/posix/ps5_mic.h). The payload SDK has no libSceAudioIn stub, so nx2/toolchain_ps5.py link_title
   builds this one as it builds PS5_Vulkan's AGC stubs: a shared object named libSceAudioIn.prx that the link and ps5-native-tool
   read to write the imports into the eboot, so that the console's loader loads the system's libSceAudioIn with the title's
   other system modules. These bodies are never packaged or run on the console.
   After mihawk-99's PS5_RetroArch, tooling/ps5-stubs/audio_in_link_stub.c (Mihawk). SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>

int sceAudioInInit(void) { return -1; }
int sceAudioInOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t param) {
    (void)user, (void)type, (void)index, (void)grain, (void)rate, (void)param;
    return -1;
}
int sceAudioInInput(int32_t port, void *pcm) {
    (void)port, (void)pcm;
    return -1;
}
int sceAudioInGetSilentState(int32_t port) {
    (void)port;
    return -1;
}
int sceAudioInClose(int32_t port) {
    (void)port;
    return -1;
}
