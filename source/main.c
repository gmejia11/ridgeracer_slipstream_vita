#include "utils/init.h"
#include "utils/glutil.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#ifndef NDK_PORT
#include "reimpl/controls.h"
#else
#include <falso_ndk/FalsoNDK.h>
#endif

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

void audio_init(void);

int main() {
    soloader_init_all();

    sceIoMkdir(DATA_PATH "files", 0777);
    sceIoMkdir(DATA_PATH "cache", 0777);
    sceIoMkdir(DATA_PATH "gxp", 0777); // compiled shader cache

    int (*JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    JNI_OnLoad(&jvm);

    // RidgeRacerActivity.onCreate() calls this before the native activity
    // starts: it stores JNIEnv and a global ref to the activity object that
    // every Java_* helper in the game relies on.
    void (*nativeInitGlobals)(JNIEnv *, jobject) = (void *)so_symbol(&so_mod,
        "Java_com_namcobandaigames_ridgeracerss_RidgeRacerActivity_nativeInitGlobals");
    nativeInitGlobals(&jni, (jobject)0x42424242);

    // The OBB state machine (DownloadingTools::eApkExpansionState) starts in
    // PAUSE (1). The Java downloader service switches it to RESUME (0) once the
    // expansion file is available, which makes CDownLoadingGUI run
    // PakCheckStart() and open the OBB. We already have the OBB, so do it now.
    void (*nativeResumeOBBDownload)(JNIEnv *, jobject) = (void *)so_symbol(&so_mod,
        "Java_com_namcobandaigames_ridgeracerss_RidgeRacerActivity_nativeResumeOBBDownload");
    nativeResumeOBBDownload(&jni, (jobject)0x42424242);

    // vitaGL's shader cache (HAVE_SHADER_CACHE) writes to
    // ux0:data/shader_cache/<TITLEID>/ but only creates the last level.
    sceIoMkdir("ux0:data/shader_cache", 0777);

    gl_init();

    // FMOD pull thread (stands in for org.fmod.FMODAudioDevice)
    audio_init();

#ifndef NDK_PORT
    // ... do some initialization

    while (1) {
        // ... render call
        gl_swap();
    }
#else
    // Build a fake ANativeActivity that the game's onCreate will receive
    ANativeActivity *activity = calloc(1, sizeof(ANativeActivity));
    activity->callbacks = calloc(1, sizeof(ANativeActivityCallbacks));
    activity->env = &jni; // from FalsoJNI
    activity->vm = &jvm;  // from FalsoJNI
    activity->clazz = (jclass)0x42424242;
    activity->internalDataPath = DATA_PATH "files";
    activity->externalDataPath = DATA_PATH "files";
    activity->sdkVersion = 19;
    activity->instance = NULL;
    activity->assetManager = (void *)0x41414141;
    activity->obbPath = DATA_PATH;

    // Drive the activity lifecycle
    int (*ANativeActivity_onCreate)(ANativeActivity *, void *, size_t) =
        (void *)so_symbol(&so_mod, "ANativeActivity_onCreate");
    ANativeActivity_onCreate(activity, NULL, 0);

    activity->callbacks->onStart(activity);
    activity->callbacks->onResume(activity);

    // Wire up input and the native window
    AInputQueue *aInputQueue = AInputQueue_create();
    activity->callbacks->onInputQueueCreated(activity, aInputQueue);

    ANativeWindow *aNativeWindow = ANativeWindow_create();
    activity->callbacks->onNativeWindowCreated(activity, aNativeWindow);

    activity->callbacks->onWindowFocusChanged(activity, 1);
#endif

    sceKernelExitDeleteThread(0);
}

#ifndef NDK_PORT
void controls_handler_key(int32_t keycode, ControlsAction action) {
    // Call into the .so here
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    // Call into the .so here
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    // Call into the .so here
}
#endif
