/* (ps5) host/hos_sdl.h on the console, in place of host/hos_sdl.c (SDL loaded at run time, which a title cannot do).
   The interface stays SDL's shape, so the callers are Linux's as they are: the window loop and the renderer's surface
   (host/posix/display_sdl.c), the controllers (host/pad_sdl.c), the sound (host/posix/win32_posix.c's waveOut).

   The controllers and the sound are the console's, as PS5_VulkanTemplate's ps5/src/platform.c (MIT, Mihawk; proven on
   the console in phase 1) drives them:
   - the controllers: sceUserServiceInitialize, scePadInit, then a player per signed-in user (sceUserServiceGetLoginUser-
     IdList, read again each time pad_sdl.c lists the pads, about once a second), each opened with scePadOpen (10 tries
     100 ms apart for the user who started the title: the pad service may not have published the device yet), read with
     scePadRead's 120-byte samples. Buttons by position (PAD_MAP), so pad_sdl.c's HOS_PAD_* (SDL's order: the bottom face
     button first) are the DualSense's; the sticks' 0..255 (128 at rest, y down) are SDL's -32768..32767 (y down too); L2
     and R2's 0..255 are SDL's triggers, 0..32767. A sample with 0x80000000 in the buttons is the shell's (the home
     screen, a dialog) and not input. Rumble: scePadSetVibration's large and small motors in the compatible mode.
   - the sound: sceAudioOutInit, sceAudioOutOpen(0xff, 0, 0, 256, 48000, S16 stereo), and a thread that pulls 256 frames
     from the host's callback (win32_posix.c's waveOut queue, which nn_audio.c fills at 48 kHz) and blocks in
     sceAudioOutOutput while the console plays them, which paces it.
   - (lot 86) the microphone: player 1's DualSense's, through sceAudioIn, with HOOKSHOT_PS5_MIC=1 (host/posix/ps5_mic.h,
     GPL-3.0-or-later, after mihawk-99's PS5_RetroArch; the hos_sdl_mic_* below).
   - the window: not yet (D1 in docs/ps5-port.md). hos_sdl_init answers 0 for HOS_SDL_VIDEO, so display_sdl.c keeps the
     headless path (the renderer offscreen) until hos_sdl_vulkan_surface makes the VK_KHR_display surface; then
     HOS_SDL_VIDEO answers 1 and display_sdl.c's loop runs as on Linux.
   - events: none (the shell closes the title; there is no window to close or resize). */
#ifdef __PROSPERO__
#include "../hos.h"
#include "../hos_sdl.h"
#include <windows.h>   /* (host/posix's: Sleep) */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ps5.h"

int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
struct hos_ps5_user_list { int32_t user_id[4]; };   /* (the signed-in users, at most four; unused entries are -1) */
int sceUserServiceGetLoginUserIdList(struct hos_ps5_user_list *list);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int scePadGetHandle(int32_t user_id, int32_t port_type, int32_t index);
int scePadRead(int32_t handle, void *samples, int32_t capacity);
int scePadReadState(int32_t handle, void *state);   /* (the latest sample, the same 120 bytes, whether or not it changed) */
int scePadClose(int32_t handle);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetVibration(int32_t handle, const void *vibration);
int scePadSetMotionSensorState(int32_t handle, _Bool enabled);
int scePadSetTiltCorrectionState(int32_t handle, _Bool enabled);
int scePadSetAngularVelocityDeadbandState(int32_t handle, _Bool enabled);
int scePadResetOrientation(int32_t handle);
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void *samples);
int sceAudioOutClose(int32_t port);
int sceKernelUsleep(uint32_t microseconds);

/* The console's pad buttons (PS5_VulkanTemplate ps5/src/platform.h, verified on hardware) in SDL's gamepad order, which
   host/pad_sdl.c reads by position (BTN_A = the bottom face button ...). */
static const uint32_t PAD_MAP[] = {
    0x4000,   /* SOUTH: cross */
    0x2000,   /* EAST: circle */
    0x8000,   /* WEST: square */
    0x1000,   /* NORTH: triangle */
    0x100000, /* BACK: the touch pad's click */
    0,        /* GUIDE: the PS button is the shell's */
    0x8,      /* START: OPTIONS */
    0x2,      /* LEFT_STICK: L3 */
    0x4,      /* RIGHT_STICK: R3 */
    0x400,    /* LEFT_SHOULDER: L1 */
    0x800,    /* RIGHT_SHOULDER: R1 */
    0x10,     /* DPAD_UP */
    0x40,     /* DPAD_DOWN */
    0x80,     /* DPAD_LEFT */
    0x20,     /* DPAD_RIGHT */
};
uint32_t hos_ps5_pad_bit(int button) { return button >= 0 && button < (int)(sizeof PAD_MAP / sizeof PAD_MAP[0]) ? PAD_MAP[button] : 0; }

static const char *last_error = "";
int hos_sdl_load(void) { return 3; }   /* (the console's calls behind SDL3's shape: always there) */
void hos_sdl_hint(const char *name, const char *value) { (void)name, (void)value; }
const char *hos_sdl_error(void) { return last_error; }
void hos_sdl_identity(const char *name, const char *app_id) { (void)name, (void)app_id; }

static int pads_init(void);
static int audio_init(void);
static void mic_start(void);
int hos_sdl_init(uint32_t subsystems) {
    if (subsystems & HOS_SDL_VIDEO) {   /* (no surface yet: display_sdl.c stays headless) */
        last_error = "the PS5 build's window (VK_KHR_display) is not made by hos_sdl_ps5.c yet";
        return 0;
    }
    if ((subsystems & HOS_SDL_GAMEPAD) && !pads_init()) return 0;
    if (subsystems & HOS_SDL_GAMEPAD) mic_start();   /* (lot 86: player 1's microphone, with HOOKSHOT_PS5_MIC=1) */
    if ((subsystems & HOS_SDL_AUDIO) && !audio_init()) return 0;
    return 1;
}

/* ---- the window: the TV, through VK_KHR_display (not yet: D1) ---- */
void *hos_sdl_window(const char *title, int width, int height) { (void)title, (void)width, (void)height; return NULL; }
void hos_sdl_fullscreen(void *window, int on) { (void)window, (void)on; }   /* (always) */
void hos_sdl_relative_mouse(void *window, int on) { (void)window, (void)on; }
int hos_sdl_desktop_size(int *width, int *height) { *width = 3840, *height = 2160; return 1; }   /* (VideoOut's 4K) */
int hos_sdl_vulkan_surface(void *window, void *instance, void *surface_out) {
    (void)window, (void)instance, (void)surface_out;
    return 0;   /* (D1: vkCreateDisplayPlaneSurfaceKHR on the first display's first mode) */
}
void hos_sdl_pixel_size(void *window, int *width, int *height) { (void)window; *width = 3840, *height = 2160; }
const uint8_t *hos_sdl_keys(int *count) { *count = 0; return NULL; }   /* (USB keyboards: later, gap 12) */
uint32_t hos_sdl_mouse_buttons(void) { return 0; }
void hos_sdl_window_size(void *window, int *width, int *height) { hos_sdl_pixel_size(window, width, height); }
void hos_sdl_icon(void *window, const char *png) { (void)window, (void)png; }   /* (sce_sys/icon0.png is the title's) */

/* ---- events: the shell ends the title, nothing else comes ---- */
int hos_sdl_wait_event(HosSdlEvent *e, int timeout_ms) {
    (void)e;
    Sleep((DWORD)(timeout_ms > 0 ? timeout_ms : 0));
    return 0;
}
int hos_sdl_poll_event(HosSdlEvent *e) { (void)e; return 0; }

/* ---- sound: sceAudioOut, 48 kHz stereo S16, 256-frame grains ---- */
#define AUDIO_GRAIN 256
#define AUDIO_ALREADY_INITIALISED ((int)0x8026000e)
static int32_t audio_port = -1;
static void (*volatile audio_pull)(void *user, uint8_t *out, int len);
static void *volatile audio_user;
static volatile int audio_running;
static pthread_mutex_t audio_mu = PTHREAD_MUTEX_INITIALIZER;   /* (audio_pull and audio_user, and the thread's use of them) */
static unsigned long long audio_grains;
static int audio_token;   /* (what hos_sdl_audio_open hands back: its address, never NULL) */
static int audio_init(void) {
    static int state;   /* 0 not tried, 1 ready, -1 refused */
    if (!state) {
        const int r = sceAudioOutInit();
        state = r >= 0 || r == AUDIO_ALREADY_INITIALISED ? 1 : -1;
        if (state < 0) {
            static char why[64];
            snprintf(why, sizeof why, "sceAudioOutInit %#x", (unsigned)r);
            last_error = why;
            fprintf(stderr, "ps5: sound: %s\n", why);
        }
    }
    return state > 0;
}
static void *audio_main(void *unused) {
    (void)unused;
    static int16_t grain[AUDIO_GRAIN * 2] __attribute__((aligned(64)));
    unsigned errors = 0;
    while (__atomic_load_n(&audio_running, __ATOMIC_ACQUIRE)) {
        /* (the pull under audio_mu: a close (waveOutClose frees the queue the pull reads right after it) waits for it; detached,
           silence keeps the port fed and paced) */
        pthread_mutex_lock(&audio_mu);
        if (audio_pull) audio_pull(audio_user, (uint8_t *)grain, (int)sizeof grain);
        else memset(grain, 0, sizeof grain);
        pthread_mutex_unlock(&audio_mu);
        /* (blocks for one grain: what paces the thread. An output that stops taking grains is said once) */
        if (sceAudioOutOutput(audio_port, grain) < 0) {
            if (errors++ == 0) fprintf(stderr, "ps5: sound: sceAudioOutOutput failed\n");
            sceKernelUsleep(5000);
        } else {
            __atomic_add_fetch(&audio_grains, 1, __ATOMIC_RELAXED);
        }
    }
    (void)sceAudioOutOutput(audio_port, NULL);   /* (the grain still queued) */
    return NULL;
}
/* (longshot #1001, #1205: open, close and alive, a token for the stream. The console has one output, the system user's main
   port, and one thread that feeds it for the whole run (an ended thread's stack is never given back here): the first open makes
   them, a close detaches the stream's pull (after the thread is out of it, as waveOutClose frees what it reads), and an open
   after a close attaches the new stream to the same port and thread. device: the console lists none (hos_sdl_output_count is
   not defined here), so it is always Default and is not looked at) */
void *hos_sdl_audio_open(unsigned device, int freq, int channels, int s16, void (*pull)(void *user, uint8_t *out, int len), void *user) {
    (void)device;
    if (!pull) return NULL;
    if (freq != 48000 || channels != 2 || !s16) {   /* (nn_audio.c renders 48 kHz S16 stereo: nothing else is asked) */
        fprintf(stderr, "ps5: sound: %d Hz, %d channels, %s asked: only 48000 Hz S16 stereo is played\n", freq, channels,
                s16 ? "S16" : "float");
        return NULL;
    }
    if (audio_port >= 0) {   /* (opened before: one stream at a time, on the same port and thread) */
        pthread_mutex_lock(&audio_mu);
        const int busy = audio_pull != NULL;
        if (!busy) audio_pull = pull, audio_user = user;
        pthread_mutex_unlock(&audio_mu);
        if (busy) return NULL;
        fprintf(stderr, "ps5: sound: the stream attached again to port %d\n", (int)audio_port);
        return &audio_token;
    }
    if (!audio_init()) return NULL;
    audio_port = sceAudioOutOpen(0xff, 0, 0, AUDIO_GRAIN, 48000, 1);   /* (the system user's main port; 1: S16 stereo) */
    if (audio_port < 0) {
        fprintf(stderr, "ps5: sound: sceAudioOutOpen %#x\n", (unsigned)audio_port);
        audio_port = -1;
        return NULL;
    }
    audio_pull = pull, audio_user = user;
    __atomic_store_n(&audio_running, 1, __ATOMIC_RELEASE);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 << 10);   /* (the platform's wrap gives it 2 MiB of direct memory) */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    const int r = pthread_create(&t, &a, audio_main, NULL);
    pthread_attr_destroy(&a);
    if (r) {
        __atomic_store_n(&audio_running, 0, __ATOMIC_RELEASE);
        audio_pull = NULL, audio_user = NULL;
        sceAudioOutClose(audio_port);
        audio_port = -1;
        fprintf(stderr, "ps5: sound: no thread (%d)\n", r);
        return NULL;
    }
    fprintf(stderr, "ps5: sound: 48 kHz stereo on port %d\n", (int)audio_port);
    return &audio_token;
}
void hos_sdl_audio_close(void *audio) {
    if (audio != &audio_token) return;
    pthread_mutex_lock(&audio_mu);   /* (the thread is out of the pull once this is held: what it read can be freed) */
    audio_pull = NULL, audio_user = NULL;
    pthread_mutex_unlock(&audio_mu);
    fprintf(stderr, "ps5: sound: the stream detached (the port and its thread stay)\n");
}
int hos_sdl_audio_alive(void *audio) {
    return audio == &audio_token && audio_port >= 0 && __atomic_load_n(&audio_running, __ATOMIC_ACQUIRE);
}
unsigned long long hos_ps5_audio_grains(void) { return __atomic_load_n(&audio_grains, __ATOMIC_RELAXED); }   /* (the probe's) */

/* ---- controllers: scePad, a player per signed-in user (PS5_VulkanTemplate's player model) ---- */
#define PLAYERS 4
#define PAD_INTERCEPTED 0x80000000u
#define PAD_VIBRATION_COMPATIBLE 2
struct pad_sample {   /* (one sample as the console writes it: 120 bytes, the layout PS5_vkQuake and ProsperoLight read) */
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y, l2, r2;
    uint8_t padding[2];
    float orientation[4];   /* (x, y, z, w: the pad's own fusion, unused: host/hos_motion.h fuses the two below itself) */
    float accel[3];         /* G (the motion sensors on: scePadSetMotionSensorState) */
    float gyro[3];          /* rad/s */
    uint8_t touch[24];
    int32_t connected;
    uint64_t timestamp_us;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t remaining[15];
};
_Static_assert(sizeof(struct pad_sample) == 120, "the console's pad samples are 120 bytes");
_Static_assert(offsetof(struct pad_sample, connected) == 0x4c, "connection state sits at 0x4c");
_Static_assert(offsetof(struct pad_sample, accel) == 0x1c && offsetof(struct pad_sample, gyro) == 0x28,
               "the motion sensors sit at 0x1c and 0x28 (SavageWind's runtime/ps5/input.c, EdGlantine's ps5_pad.hpp)");
/* A player keeps its handle while its user is signed in: pad_sdl.c's open and close (a controller that comes and goes)
   only say whether it reads it, they never close the handle (closing and reopening it once a second made the controller
   flap in the platform probe's run 3, when its samples said "not connected": nobody was holding it). Whether a controller
   is there is what its samples say, with a grace period: absent only after HOOKSHOT_PS5_PAD_GRACE_MS (2000) with no
   sample saying connected, or with reads failing. */
typedef struct {
    int32_t user, handle;
    int open;               /* pad_sdl.c holds it */
    int64_t seen_ns;        /* the last sample that said connected (0: none yet) */
    int64_t opened_ns;      /* when the handle was opened */
    struct pad_sample last;
    char name[32];
    int sensors;            /* the motion sensors pad_sdl.c asked on: 1 << HOS_SDL_SENSOR_ACCEL | 1 << ..._GYRO */
    int32_t motion_handle;  /* the handle scePadSetMotionSensorState turned them on for (-1 none) */
    int motion_said;        /* the raw readings' log lines given (the axes' check: see hos_sdl_pad_sensor) */
    int64_t motion_on_ns;   /* when they were turned on */
} Player;
static Player players[PLAYERS];
static int pad_service;
static int32_t initial_user = -1, unopened_user = -1;
static pthread_mutex_t pads_mu = PTHREAD_MUTEX_INITIALIZER;
static struct hos_ps5_pad_stats pad_st;

static int64_t pad_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}
static int64_t grace_ns(void) {
    static int64_t g;
    if (!g) {
        const char *e = getenv("HOOKSHOT_PS5_PAD_GRACE_MS");
        g = (int64_t)(e && atoi(e) > 0 ? atoi(e) : 2000) * 1000000ll;
    }
    return g;
}
/* there now: a sample said connected within the grace period */
static int present(const Player *p) { return p->handle >= 0 && p->seen_ns && pad_now_ns() - p->seen_ns < grace_ns(); }

static void player_rest(Player *p) {
    memset(p, 0, sizeof *p);
    p->user = p->handle = p->motion_handle = -1;
    p->last.left_x = p->last.left_y = p->last.right_x = p->last.right_y = 128;
}
static int player_open(int i, int32_t user, int tries) {
    Player *const p = &players[i];
    player_rest(p);
    int32_t h = -1;
    for (int t = 0; t < tries && h < 0; t++) {   /* (a title can start before the pad service has published the device) */
        h = scePadOpen(user, 0, 0, NULL);
        if (h < 0 && t + 1 < tries) sceKernelUsleep(100000);
    }
    if (h < 0) h = scePadGetHandle(user, 0, 0);   /* (one this process already holds for the user) */
    if (h < 0) {
        if (user != unopened_user) fprintf(stderr, "ps5: pad: scePadOpen failed for user %d (%#x)\n", (int)user, (unsigned)h);
        unopened_user = user;
        return 0;
    }
    p->user = user, p->handle = h, p->opened_ns = pad_now_ns();
    snprintf(p->name, sizeof p->name, "DualSense (player %d)", i + 1);
    __atomic_add_fetch(&pad_st.handles_opened, 1, __ATOMIC_RELAXED);
    fprintf(stderr, "ps5: pad: player %d opened for user %d, rumble mode %d\n", i + 1, (int)user,
            scePadSetVibrationMode(h, PAD_VIBRATION_COMPATIBLE));
    return 1;
}
/* every player's new samples (pads_mu held) */
static void players_read(void) {
    static struct pad_sample samples[64];
    for (int i = 0; i < PLAYERS; i++) {
        Player *const p = &players[i];
        if (p->handle < 0) continue;
        int n = scePadRead(p->handle, samples, 64);
        pad_st.reads++;
        if (n < 0) { pad_st.read_errors++, pad_st.last_error = n; continue; }
        if (n == 0) {   /* (nothing new: an idle controller is still there, which the latest state says) */
            static union { struct pad_sample s; uint8_t room[256]; } latest;
            const int r = scePadReadState(p->handle, &latest);
            pad_st.states++;
            if (r < 0) { pad_st.read_errors++, pad_st.last_error = r; continue; }
            samples[0] = latest.s, n = 1;
        }
        for (int k = 0; k < n && k < 64; k++) {
            pad_st.samples++;
            if (samples[k].buttons & PAD_INTERCEPTED) pad_st.intercepted++;
            if (!samples[k].connected) { pad_st.not_connected++; continue; }
            p->seen_ns = pad_now_ns();
            if (!(samples[k].buttons & PAD_INTERCEPTED)) p->last = samples[k];
            else memset(p->last.gyro, 0, sizeof p->last.gyro);   /* (the shell has the pad: the aim stops, it does not
                                                                     keep the last turn) */
        }
    }
}
/* the signed-in users again: player 0 stays the initial user's, a user who signed out is let go (the handle closed), a new
   one takes the first free player (pads_mu held) */
static void pads_scan(void) {
    struct hos_ps5_user_list list = {{-1, -1, -1, -1}};
    if (sceUserServiceGetLoginUserIdList(&list) < 0) return;
    for (int i = 1; i < PLAYERS; i++) {
        if (players[i].handle < 0) continue;
        int in = 0;
        for (int k = 0; k < 4; k++) in |= list.user_id[k] >= 0 && list.user_id[k] == players[i].user;
        if (!in) {
            fprintf(stderr, "ps5: pad: player %d let go (user %d signed out)\n", i + 1, (int)players[i].user);
            scePadClose(players[i].handle);
            player_rest(&players[i]);
        }
    }
    for (int k = 0; k < 4; k++) {
        const int32_t user = list.user_id[k];
        int known = user < 0;
        for (int i = 0; i < PLAYERS && !known; i++) known = players[i].handle >= 0 && players[i].user == user;
        for (int i = 0; i < PLAYERS && !known; i++)
            if (players[i].handle < 0) {
                if (i == 0 && user != initial_user) continue;   /* (player 0 is the initial user's) */
                player_open(i, user, 1);
                break;
            }
    }
}
static int pads_init(void) {
    static int state;   /* 0 not tried, 1 ready, -1 refused */
    if (state) return state > 0;
    pthread_mutex_lock(&pads_mu);
    for (int i = 0; i < PLAYERS; i++) player_rest(&players[i]);
    (void)sceUserServiceInitialize(NULL);
    const int u = sceUserServiceGetInitialUser(&initial_user), s = scePadInit();
    if (u < 0 || s < 0) {
        static char why[80];
        snprintf(why, sizeof why, "no user (%#x) or no pad service (%#x)", (unsigned)u, (unsigned)s);
        last_error = why;
        fprintf(stderr, "ps5: pad: %s\n", why);
        state = -1;
    } else {
        pad_service = 1, state = 1;
        player_open(0, initial_user, 10);
        pads_scan();
    }
    pthread_mutex_unlock(&pads_mu);
    return state > 0;
}

/* (v2.0.11's sound: the game is told what the output has. One S16 stereo port is opened here, so 2: nn_audio.c then mixes
   its six channels down. 5.1 on the console would be a port of AUDIO_OUT's 8-channel format: not done) */
int hos_sdl_audio_channels(void) { return 2; }
/* (lot 86) v2.0.11's microphones (host/nn_audio_in.c, the ocarina played by voice or humming): player 1's DualSense
   microphone through sceAudioIn, with HOOKSHOT_PS5_MIC=1 in hookshot_env.txt. host/posix/ps5_mic.h (GPL-3.0-or-later, after
   mihawk-99's PS5_RetroArch src/microphone_ps5.cpp) does the work; here are the console's calls it is given. The list has one
   microphone, "DualSense microphone", while player 1's controller is there; the default (id 0) is the same one. Without the
   switch the game is told no recording device is there, as on a PC without one, and nothing calls sceAudioIn: only the
   module comes in with the title's other system modules (ps5/stubs/libSceAudioIn_link_stub.c). The switch is read at the
   first call, from hos_sdl_init's controllers (after main, stage 4's settings in the environment) or the game's first
   microphone; HOOKSHOT_PS5_MIC_TYPE=0: the voice-chat port type tried before the general one */
int sceAudioInInit(void);
int sceAudioInOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t param);
int sceAudioInInput(int32_t port, void *pcm);
int sceAudioInGetSilentState(int32_t port);
int sceAudioInClose(int32_t port);
#include "ps5_mic.h"
#ifndef LONGSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../../gpu/vk/compat/longshot_env.h"
#endif
static int32_t mic_user(void) {   /* (player 1's, the user who started the title) */
    pthread_mutex_lock(&pads_mu);
    const int32_t user = players[0].handle >= 0 ? players[0].user : initial_user;
    pthread_mutex_unlock(&pads_mu);
    return user;
}
static int mic_player(void) {
    pthread_mutex_lock(&pads_mu);
    const int there = present(&players[0]);
    pthread_mutex_unlock(&pads_mu);
    return there;
}
static int mic_thread(void *(*main)(void *)) {   /* (once for the run, as the sound's: an ended thread's stack is not given back) */
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 << 10);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    const int r = pthread_create(&t, &a, main, NULL);
    pthread_attr_destroy(&a);
    return r;
}
static const char *mic_memory(char *buf, size_t cap) {
    uint64_t direct = 0, flexible = 0, largest = 0;
    if (hos_ps5_memory_free(&direct, &flexible, &largest))
        snprintf(buf, cap, " (flexible %llu MiB free, direct %llu MiB free)", (unsigned long long)flexible, (unsigned long long)direct);
    else
        snprintf(buf, cap, " (memory: no answer)");
    return buf;
}
static const HosPs5MicOps *mic_ops(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static HosPs5MicOps o;
    static int made;
    pthread_mutex_lock(&mu);
    if (!made) {
        const char *type = getenv("HOOKSHOT_PS5_MIC_TYPE");
        o = (HosPs5MicOps){sceAudioInInit, sceAudioInOpen, sceAudioInInput, sceAudioInGetSilentState, sceAudioInClose, mic_user,
                           mic_player, mic_thread, pad_now_ns, hos_log, mic_memory, env_on("HOOKSHOT_PS5_MIC"),
                           !(type && type[0] == '0')};
        made = 1;
    }
    pthread_mutex_unlock(&mu);
    return &o;
}
static void mic_start(void) {   /* (hos_sdl_init's controllers: the port made early, before the game's memory is taken) */
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    hos_ps5_mic_prepare(mic_ops());
}
int hos_sdl_mic_list(uint32_t *ids, char (*names)[128], int max) { return hos_ps5_mic_list(mic_ops(), ids, names, max); }
void *hos_sdl_mic_open(uint32_t id, int freq, int channels, uint16_t sdl_format) {
    void *const mic = hos_ps5_mic_open(mic_ops(), id, freq, channels, sdl_format);
    if (!mic) last_error = hos_ps5_mic_error();
    return mic;
}
int hos_sdl_mic_available(void *mic) { return hos_ps5_mic_available(mic); }
int hos_sdl_mic_read(void *mic, void *dst, int bytes) { return hos_ps5_mic_read(mic, dst, bytes); }
void hos_sdl_mic_clear(void *mic) { hos_ps5_mic_clear(mic); }
void hos_sdl_mic_close(void *mic) { hos_ps5_mic_close(mic); }
/* (host/posix/ps5_relaunch.c, the title closing from the menu: the port closed, the capture thread ended) */
void hos_ps5_mic_end(void) { hos_ps5_mic_shutdown(); }

int hos_sdl_pads_list(int32_t *ids, int *opens, int max) {
    if (!pad_service) return 0;
    pthread_mutex_lock(&pads_mu);
    pads_scan();   /* (pad_sdl.c lists them about once a second: the users are read again then) */
    players_read();
    int n = 0;
    for (int i = 0; i < PLAYERS && n < max; i++)
        if (present(&players[i])) ids[n] = players[i].user, opens[n] = i, n++;
    pthread_mutex_unlock(&pads_mu);
    return n;
}
void *hos_sdl_pad_open(int what) {
    if (what < 0 || what >= PLAYERS || players[what].handle < 0) return NULL;
    __atomic_add_fetch(&pad_st.opens, 1, __ATOMIC_RELAXED);
    players[what].open = 1;
    players[what].sensors = 0;   /* (pad_sdl.c asks for the motion sensors afresh after each open: they are turned on again) */
    return &players[what];
}
void hos_sdl_pad_close(void *pad) {   /* (pad_sdl.c saw it go: read no more until it is back; the handle stays) */
    Player *const p = (Player *)pad;
    if (!p) return;
    __atomic_add_fetch(&pad_st.closes, 1, __ATOMIC_RELAXED);
    p->open = 0;
}
int hos_sdl_pad_connected(void *pad) { return pad && present((Player *)pad); }
void hos_sdl_pads_update(void) {
    pthread_mutex_lock(&pads_mu);
    players_read();
    pthread_mutex_unlock(&pads_mu);
}
void hos_ps5_pad_stats(struct hos_ps5_pad_stats *out) {
    pthread_mutex_lock(&pads_mu);
    *out = pad_st;
    out->present = 0;
    for (int i = 0; i < PLAYERS; i++) out->present += present(&players[i]);
    pthread_mutex_unlock(&pads_mu);
}
int hos_sdl_pad_button(void *pad, int button) {
    const Player *const p = (const Player *)pad;
    const uint32_t bit = hos_ps5_pad_bit(button);
    return p && bit && present(p) && (p->last.buttons & bit) != 0;
}
static int16_t stick(uint8_t v) {   /* 0..255, 128 at rest -> -32768..32767 */
    const int s = ((int)v - 128) * 256 + ((int)v - 128) / 2;
    return (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
}
int16_t hos_sdl_pad_axis(void *pad, int axis) {
    const Player *const p = (const Player *)pad;
    if (!p || !present(p)) return 0;
    switch (axis) {   /* (SDL's order: LEFTX, LEFTY, RIGHTX, RIGHTY, LEFT_TRIGGER, RIGHT_TRIGGER; y down on both) */
    case 0: return stick(p->last.left_x);
    case 1: return stick(p->last.left_y);
    case 2: return stick(p->last.right_x);
    case 3: return stick(p->last.right_y);
    case 4: return (int16_t)(p->last.l2 * 32767 / 255);
    case 5: return (int16_t)(p->last.r2 * 32767 / 255);
    default: return 0;
    }
}
const char *hos_sdl_pad_name(void *pad) { return pad ? ((Player *)pad)->name : "DualSense"; }
int32_t hos_sdl_pad_id(void *pad) { return pad ? ((Player *)pad)->user : 0; }
void hos_sdl_pad_rumble(void *pad, uint16_t low, uint16_t high, uint32_t ms) {
    (void)ms;   /* (the compatible mode holds a level until the next: pad_sdl.c sends 0 to stop, and again every 500 ms) */
    const Player *const p = (const Player *)pad;
    if (!p || p->handle < 0) return;
    const uint8_t motors[2] = {(uint8_t)(low >> 8), (uint8_t)(high >> 8)};   /* (large: the low band, small: the high) */
    (void)scePadSetVibration(p->handle, motors);
}
/* (2026-11-18) The DualSense's motion sensors to v2.0.11's motion controls (host/pad_sdl.c, host/hos_motion.h): the game's
   gyro aiming, as on a PC with a DualSense through SDL. Each sample carries them (struct pad_sample: acceleration in G,
   angular velocity in rad/s) once scePadSetMotionSensorState turns them on. As SavageWind's PS5 port (runtime/ps5/input.c)
   and EdGlantine's (eden/headless/pad.cpp) do:
   - the pad's own tilt correction and angular-velocity deadband are turned off (hos_motion.h learns the gyroscope's resting
     rate and has its own deadzone: the console's would swallow a slow aim), and its orientation is not used (hos_motion.h
     fuses the two sensors itself, so nothing is fused twice);
   - the axes are taken as SDL's (+X right, +Y out of the face buttons, +Z toward the player), the units made SDL's
     (m/s^2, rad/s). NOT YET SEEN ON THIS CONSOLE: neither port says it was checked on a held pad. The check is the
     "ps5: pad: motion:" lines below: the pad lying face up on a table reads about (0, +1, 0) G.
   HOOKSHOT_PS5_GYRO=0: no sensor reported, as before (the game sees the pad lying still). The Insert menu's Motion controls
   row, and its sensitivity and invert rows, apply as on a PC. */
static int gyro_allowed(void) {
    static int allowed = -1;
    if (allowed < 0) {
        const char *e = getenv("HOOKSHOT_PS5_GYRO");
        allowed = !(e && e[0] == '0');
        if (!allowed) hos_log("ps5: pad: motion sensors not reported (HOOKSHOT_PS5_GYRO=0)");
    }
    return allowed;
}
static int sensor_type(int type) { return type == HOS_SDL_SENSOR_ACCEL || type == HOS_SDL_SENSOR_GYRO; }
int hos_sdl_pad_has_sensor(void *pad, int type) {
    const Player *const p = (const Player *)pad;
    return p && p->handle >= 0 && sensor_type(type) && gyro_allowed();
}
int hos_sdl_pad_sensor_enable(void *pad, int type, int on) {
    Player *const p = (Player *)pad;
    if (!p || p->handle < 0 || !sensor_type(type) || !gyro_allowed()) return 0;
    const int want = on ? p->sensors | 1 << type : p->sensors & ~(1 << type);
    if ((want != 0) != (p->sensors != 0) || (want && p->motion_handle != p->handle)) {   /* (one switch for both sensors) */
        const int r = scePadSetMotionSensorState(p->handle, want != 0);
        if (r < 0) {
            hos_log("ps5: pad: %s: motion sensors not turned %s (%#x)", p->name, want ? "on" : "off", (unsigned)r);
            return 0;
        }
        if (want) {
            const int rt = scePadSetTiltCorrectionState(p->handle, 0), rd = scePadSetAngularVelocityDeadbandState(p->handle, 0),
                      ro = scePadResetOrientation(p->handle);
            p->motion_handle = p->handle, p->motion_said = 0, p->motion_on_ns = pad_now_ns();
            hos_log("ps5: pad: %s: motion sensors on (tilt correction off %#x, angular-velocity deadband off %#x, "
                    "orientation reset %#x)", p->name, (unsigned)rt, (unsigned)rd, (unsigned)ro);
        } else {
            p->motion_handle = -1;
            hos_log("ps5: pad: %s: motion sensors off", p->name);
        }
    }
    p->sensors = want;
    return 1;
}
int hos_sdl_pad_sensor(void *pad, int type, float *data, int n) {
    Player *const p = (Player *)pad;
    if (!p || !data || n < 3 || !sensor_type(type) || !(p->sensors & 1 << type) || p->motion_handle != p->handle ||
        !present(p))
        return 0;
    const float *const v = type == HOS_SDL_SENSOR_ACCEL ? p->last.accel : p->last.gyro;
    const float scale = type == HOS_SDL_SENSOR_ACCEL ? 9.80665f : 1.0f;   /* (G to SDL's m/s^2; rad/s as SDL has them) */
    for (int i = 0; i < 3; i++) data[i] = v[i] * scale;
    /* (the axes' check: the raw readings at 3 s and 20 s after the sensors came on, and what the pad lying face up reads) */
    if (type == HOS_SDL_SENSOR_GYRO && p->motion_said < 2 &&
        pad_now_ns() - p->motion_on_ns > (p->motion_said ? 20000000000ll : 3000000000ll)) {
        p->motion_said++;
        hos_log("ps5: pad: motion: %s: accel (%+.3f, %+.3f, %+.3f) G, gyro (%+.4f, %+.4f, %+.4f) rad/s, orientation "
                "(%+.3f, %+.3f, %+.3f, %+.3f) (lying face up on a table the accel reads about (0, +1, 0) when the axes "
                "are SDL's)", p->name, p->last.accel[0], p->last.accel[1], p->last.accel[2], p->last.gyro[0],
                p->last.gyro[1], p->last.gyro[2], p->last.orientation[0], p->last.orientation[1], p->last.orientation[2],
                p->last.orientation[3]);
    }
    return 1;
}
/* (upstream v2.2.0's calls, host/hos_sdl.h: HD rumble's pad type and raw effects, Steam's stand-in pads, the connection's log
   line. The console's pad is a DualSense through scePad: none of those, the plain rumble above as before) */
int hos_sdl_hint_override(const char *name, const char *value) { (void)name, (void)value; return 0; }
int hos_sdl_knows_steam_pads(void) { return 0; }
uint16_t hos_sdl_pad_vendor(void *pad) { (void)pad; return 0; }
uint64_t hos_sdl_pad_steam_handle(void *pad) { (void)pad; return 0; }
void hos_sdl_pad_log_connection(void *pad, int motion, int steam) { (void)pad, (void)motion, (void)steam; }
int hos_sdl_pad_type(void *pad) { (void)pad; return 0; }
int hos_sdl_pad_has_effect(void) { return 0; }
int hos_sdl_pad_effect(void *pad, const void *data, int size) { (void)pad, (void)data, (void)size; return 0; }
#endif /* __PROSPERO__ */
