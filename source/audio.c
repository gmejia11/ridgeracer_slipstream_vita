/*
 * audio.c -- FMOD Ex (Android "AudioTrack" output) -> sceAudioOut bridge
 *
 * On Android, FMOD Ex's AudioTrack output (FMOD_OUTPUTTYPE_AUDIOTRACK = 21)
 * is passive: the Java class org.fmod.FMODAudioDevice runs a thread that
 * pulls PCM from the native mixer through two JNI exports:
 *   int fmodGetInfo(int what)  0 = sample rate, 1 = DSP buffer length,
 *                              2 = DSP num buffers, 3 = mixer running (1)
 *   int fmodProcess(ByteBuffer) mixes one DSP block (stereo s16) into buffer
 * We reproduce that thread here and feed the result to sceAudioOut.
 */

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <string.h>
#include <stdlib.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/settings.h"
#include <psp2/kernel/processmgr.h>
void perf_audio_mix(uint64_t us);

extern so_module so_mod, fmodex_mod, fmodevent_mod;

#define FMOD_OUTPUTTYPE_AUDIOTRACK 21
#define AUDIO_GRAIN 512 // samples per sceAudioOutOutput call

static int  (*fmodGetInfo)(JNIEnv *, jobject, int);
static int  (*fmodProcess)(JNIEnv *, jobject, jobject);

/*
 * FalsoJNI does not implement direct ByteBuffers. We hand FMOD a private
 * JNIEnv whose GetDirectBufferAddress returns the "buffer object" itself,
 * which is just a raw pointer to our PCM buffer.
 */
static struct JNINativeInterface audio_iface;
static JNIEnv audio_env;

static void *audio_GetDirectBufferAddress(JNIEnv *env, jobject buf) {
    return (void *)buf;
}

static int audio_thread(SceSize argc, void *argv) {
    int16_t *mix = NULL;     // FMOD block
    int16_t *ring = NULL;    // accumulates FMOD blocks
    int ring_fill = 0;       // in stereo frames
    int port = -1;
    int rate = 0, block = 0;

    while (1) {
        if (port < 0) {
            if (fmodGetInfo(&audio_env, NULL, 3) != 1) {
                sceKernelDelayThread(10 * 1000);
                continue;
            }

            rate = fmodGetInfo(&audio_env, NULL, 0);
            block = fmodGetInfo(&audio_env, NULL, 1);
            if (rate <= 0 || block <= 0) {
                sceKernelDelayThread(10 * 1000);
                continue;
            }

            l_info("FMOD mixer running: %d Hz, block %d frames", rate, block);

            mix = malloc(block * 2 * sizeof(int16_t));
            ring = malloc((block + AUDIO_GRAIN) * 2 * sizeof(int16_t) * 2);
            ring_fill = 0;

            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN,
                                       rate, SCE_AUDIO_OUT_MODE_STEREO);
            if (port < 0) {
                l_error("sceAudioOutOpenPort(%d Hz) failed: 0x%08x, retrying at 48000", rate, port);
                port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN,
                                           48000, SCE_AUDIO_OUT_MODE_STEREO);
                if (port < 0) {
                    l_fatal("Could not open audio port: 0x%08x", port);
                    return 0;
                }
            }

            int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
            sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
        }

        while (ring_fill < AUDIO_GRAIN) {
            memset(mix, 0, block * 2 * sizeof(int16_t));
#ifdef PERF_PROFILE
            uint64_t t0 = sceKernelGetProcessTimeWide();
            fmodProcess(&audio_env, NULL, (jobject)mix);
            perf_audio_mix(sceKernelGetProcessTimeWide() - t0);
#else
            fmodProcess(&audio_env, NULL, (jobject)mix);
#endif
            memcpy(ring + ring_fill * 2, mix, block * 2 * sizeof(int16_t));
            ring_fill += block;
        }

        sceAudioOutOutput(port, ring);

        ring_fill -= AUDIO_GRAIN;
        if (ring_fill > 0)
            memmove(ring, ring + AUDIO_GRAIN * 2, ring_fill * 2 * sizeof(int16_t));
    }

    return 0;
}

/*
 * Force FMOD to the AudioTrack output before EventSystem::init().
 */
static so_hook eventsystem_init_hook;
static int (*EventSystem_getSystemObject)(void *self, void **system);
static int (*System_setOutput)(void *system, int output);

int EventSystem_init_hook(void *self, int maxchannels, unsigned int flags,
                          void *extradriverdata, unsigned int eventflags) {
    void *system = NULL;
    if (EventSystem_getSystemObject(self, &system) == 0 && system) {
        int r = System_setOutput(system, FMOD_OUTPUTTYPE_AUDIOTRACK);
        l_info("FMOD System::setOutput(AUDIOTRACK) = %d", r);
    }

    int ret = SO_CONTINUE(int, eventsystem_init_hook, self, maxchannels, flags,
                          extradriverdata, eventflags);
    l_info("FMOD EventSystem::init(%d, 0x%x, %p, 0x%x) = %d", maxchannels, flags,
           extradriverdata, eventflags, ret);
    return ret;
}

void fmod_patch(void) {
    fmodGetInfo = (void *)so_symbol(&fmodex_mod, "Java_org_fmod_FMODAudioDevice_fmodGetInfo");
    fmodProcess = (void *)so_symbol(&fmodex_mod, "Java_org_fmod_FMODAudioDevice_fmodProcess");
    System_setOutput = (void *)so_symbol(&fmodex_mod, "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE");
    EventSystem_getSystemObject = (void *)so_symbol(&fmodevent_mod,
        "_ZN4FMOD11EventSystem15getSystemObjectEPPNS_6SystemE");

    uintptr_t init = so_symbol(&fmodevent_mod, "_ZN4FMOD11EventSystem4initEijPvj");
    if (!fmodGetInfo || !fmodProcess || !System_setOutput || !EventSystem_getSystemObject || !init) {
        l_fatal("fmod_patch: missing FMOD symbols!");
        return;
    }
    eventsystem_init_hook = hook_addr(init, (uintptr_t)&EventSystem_init_hook);
}

void audio_init(void) {
    audio_iface = *jni;
    audio_iface.GetDirectBufferAddress = audio_GetDirectBufferAddress;
    audio_env = &audio_iface;

    SceUID thid = sceKernelCreateThread("fmod_audio", audio_thread, 64, 64 * 1024, 0,
                                       setting_cores ? SCE_KERNEL_CPU_MASK_USER_1 : 0, NULL);
    if (thid < 0) {
        l_fatal("Could not create audio thread: 0x%08x", thid);
        return;
    }
    sceKernelStartThread(thid, 0, NULL);
}
