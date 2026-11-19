/*
 * java.c -- FalsoJNI implementation for Ridge Racer Slipstream
 *
 * Methods called by libRidgeRacer.so on the Java side
 * (com.namcobandaigames.ridgeracerss.RidgeRacerActivity and
 *  com.namcobandaigames.ridgeracerss.RRSSAndroidNwInputControllerDelegate).
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <so_util/so_util.h>
#include <string.h>
#include <stdio.h>

#include <psp2/ctrl.h>

#include "utils/logger.h"

extern so_module so_mod;

/*
 * Controller state, filled by controls_poll_gamepad() (see controls_rr.c)
 */
extern int   rr_pad_connected;
extern int   rr_key_down(int android_keycode);
extern float rr_axis_value(int android_axis);

/* ---------------- Object-returning methods ---------------- */

static char dummy_obj[16];

jobject GetExpansionFileNameWithPath(jmethodID id, va_list args) {
    l_info("GetExpansionFileNameWithPath -> %s", OBB_PATH);
    return (jobject)jni->NewStringUTF(&jni, OBB_PATH);
}

jobject GetUDID(jmethodID id, va_list args) {
    return (jobject)jni->NewStringUTF(&jni, "00000000-0000-0000-0000-00000000VITA");
}

jobject GetVersionName(jmethodID id, va_list args) {
    return (jobject)jni->NewStringUTF(&jni, "2.5.4");
}

jobject GetDeviceName(jmethodID id, va_list args) {
    return (jobject)jni->NewStringUTF(&jni, "Sony PS Vita");
}

// activity.getClassLoader() / classLoader.loadClass(name)
jobject getClassLoader(jmethodID id, va_list args) {
    return (jobject)dummy_obj;
}

jobject loadClass(jmethodID id, va_list args) {
    jstring name = va_arg(args, jstring);
    const char *s = jni->GetStringUTFChars(&jni, name, NULL);
    l_info("loadClass(%s)", s ? s : "(null)");
    jobject ret = (jobject)jni->FindClass(&jni, s ? s : "unknown");
    if (s) jni->ReleaseStringUTFChars(&jni, name, s);
    return ret;
}

// RRSSAndroidNwInputControllerDelegate.getInstance()
jobject getInstance(jmethodID id, va_list args) {
    return (jobject)dummy_obj;
}

/* ---------------- Int methods ---------------- */

jint GetVersionCode(jmethodID id, va_list args) {
    // Must match the OBB version (main.2319...), otherwise the game deletes
    // the OBB and tries to re-download it.
    return 2319;
}

jint GetOrientation(jmethodID id, va_list args) {
    return 0; // Surface.ROTATION_0, Vita is landscape-native
}

// Controller delegate:
//   getState(1)       -> 1 when a controller is connected
//   getKeyCode(code)  -> KeyEvent action: 0 = ACTION_DOWN, 1 = ACTION_UP
//   getInfo(i)        -> unused here
jint getState(jmethodID id, va_list args) {
    int what = va_arg(args, int);
    if (what == 1)
        return rr_pad_connected ? 1 : 0;
    return 0;
}

jint getKeyCode(jmethodID id, va_list args) {
    int code = va_arg(args, int);
    return rr_key_down(code) ? 0 : 1;
}

jint getInfo(jmethodID id, va_list args) {
    return 0;
}

/* ---------------- Float methods ---------------- */

jfloat getAxisValue(jmethodID id, va_list args) {
    int axis = va_arg(args, int);
    return rr_axis_value(axis);
}

/* ---------------- Boolean methods ---------------- */

jboolean controller_init(jmethodID id, va_list args) {
    l_info("Controller delegate init()");
    return JNI_TRUE;
}

jboolean isOnline(jmethodID id, va_list args) {
    return JNI_FALSE;
}

/* ---------------- Void methods ---------------- */

void void_stub(jmethodID id, va_list args) {
}

void PlayIntroVideo(jmethodID id, va_list args) {
    // No video playback: tell the splash screen to continue right away.
    void (*ContinueSplashUpdate)(JNIEnv *, jobject) = (void *)so_symbol(&so_mod,
        "Java_com_namcobandaigames_ridgeracerss_RidgeRacerActivity_ContinueSplashUpdate");
    if (ContinueSplashUpdate)
        ContinueSplashUpdate(&jni, (jobject)0x42424242);
}

void ExpansionDownload(jmethodID id, va_list args) {
    l_error("ExpansionDownload() requested! OBB at %s is missing or invalid.", OBB_PATH);
}

/*
 * JNI Methods table
 */

NameToMethodID nameToMethodId[] = {
    // Objects
    { 1,  "GetExpansionFileNameWithPath", METHOD_TYPE_OBJECT },
    { 2,  "GetUDID",                      METHOD_TYPE_OBJECT },
    { 3,  "GetVersionName",               METHOD_TYPE_OBJECT },
    { 4,  "GetDeviceName",                METHOD_TYPE_OBJECT },
    { 5,  "getClassLoader",               METHOD_TYPE_OBJECT },
    { 6,  "loadClass",                    METHOD_TYPE_OBJECT },
    { 7,  "getInstance",                  METHOD_TYPE_OBJECT },

    // Ints
    { 20, "GetVersionCode",               METHOD_TYPE_INT },
    { 21, "GetOrientation",               METHOD_TYPE_INT },
    { 22, "getState",                     METHOD_TYPE_INT },
    { 23, "getKeyCode",                   METHOD_TYPE_INT },
    { 24, "getInfo",                      METHOD_TYPE_INT },

    // Floats
    { 30, "getAxisValue",                 METHOD_TYPE_FLOAT },

    // Booleans
    { 40, "init",                         METHOD_TYPE_BOOLEAN },
    { 41, "isOnline",                     METHOD_TYPE_BOOLEAN },

    // Voids
    { 50, "PlayIntroVideo",               METHOD_TYPE_VOID },
    { 51, "ExpansionDownload",            METHOD_TYPE_VOID },
    { 52, "exit",                         METHOD_TYPE_VOID },
    { 53, "onPause",                      METHOD_TYPE_VOID },
    { 54, "onResume",                     METHOD_TYPE_VOID },
    { 55, "ShowPlaynomicsPlacement",      METHOD_TYPE_VOID },
    { 56, "LogAnaliticsEventWithPlaynomics", METHOD_TYPE_VOID },
    { 57, "ApplovinAd",                   METHOD_TYPE_VOID },
    { 58, "PlayHavenAd",                  METHOD_TYPE_VOID },
    { 59, "ChartboostAd",                 METHOD_TYPE_VOID },
    { 60, "TakeScreenshotAndShareIt",     METHOD_TYPE_VOID },
    { 61, "SamsungAccountActivity",       METHOD_TYPE_VOID },
    { 62, "StartNotification",            METHOD_TYPE_VOID },
    { 63, "CancelNotification",           METHOD_TYPE_VOID },
    { 64, "ShowInfoBox",                  METHOD_TYPE_VOID },
    { 65, "HideInfoBox",                  METHOD_TYPE_VOID },
    { 66, "ShowInputTextDialog",          METHOD_TYPE_VOID },
    { 67, "HideKeyboard",                 METHOD_TYPE_VOID },
    { 68, "OpenURL",                      METHOD_TYPE_VOID },
    { 69, "SendEmail",                    METHOD_TYPE_VOID },
    { 70, "LogFlurryEventWithParameter",  METHOD_TYPE_VOID },
    { 71, "AddParametersToFlurryMap",     METHOD_TYPE_VOID },
    { 72, "LogFlurryEventWithoutParameter", METHOD_TYPE_VOID },
    { 73, "ServerSyncTime",               METHOD_TYPE_VOID },
    { 74, "GoogleGameServicesSubmitScore", METHOD_TYPE_VOID },
    { 75, "loginNFacebook",               METHOD_TYPE_VOID },
    { 76, "logoutFacebook",               METHOD_TYPE_VOID },
    { 77, "likeFacebook",                 METHOD_TYPE_VOID },
    { 78, "refreshFriendsFacebook",       METHOD_TYPE_VOID },
    { 79, "postFacebook",                 METHOD_TYPE_VOID },
    { 80, "GoogleInitGameServices",       METHOD_TYPE_VOID },
    { 81, "GoogleShowGameServiceWindow",  METHOD_TYPE_VOID },
    { 82, "GoogleGameServicesUnlockAchievement", METHOD_TYPE_VOID },
    { 83, "GoogleSignIn",                 METHOD_TYPE_VOID },
    { 84, "GoogleSignOut",                METHOD_TYPE_VOID },
    { 85, "GoogleInitStore",              METHOD_TYPE_VOID },
    { 86, "GoogleStartPurchase",          METHOD_TYPE_VOID },
    { 87, "GoogleQueryProducts",          METHOD_TYPE_VOID },
};

MethodsBoolean methodsBoolean[] = {
    { 40, controller_init },
    { 41, isOnline },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
    { 30, getAxisValue },
};
MethodsInt methodsInt[] = {
    { 20, GetVersionCode },
    { 21, GetOrientation },
    { 22, getState },
    { 23, getKeyCode },
    { 24, getInfo },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { 1, GetExpansionFileNameWithPath },
    { 2, GetUDID },
    { 3, GetVersionName },
    { 4, GetDeviceName },
    { 5, getClassLoader },
    { 6, loadClass },
    { 7, getInstance },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { 50, PlayIntroVideo },
    { 51, ExpansionDownload },
    { 52, void_stub }, { 53, void_stub }, { 54, void_stub }, { 55, void_stub },
    { 56, void_stub }, { 57, void_stub }, { 58, void_stub }, { 59, void_stub },
    { 60, void_stub }, { 61, void_stub }, { 62, void_stub }, { 63, void_stub },
    { 64, void_stub }, { 65, void_stub }, { 66, void_stub }, { 67, void_stub },
    { 68, void_stub }, { 69, void_stub }, { 70, void_stub }, { 71, void_stub },
    { 72, void_stub }, { 73, void_stub }, { 74, void_stub }, { 75, void_stub },
    { 76, void_stub }, { 77, void_stub }, { 78, void_stub }, { 79, void_stub },
    { 80, void_stub }, { 81, void_stub }, { 82, void_stub }, { 83, void_stub },
    { 84, void_stub }, { 85, void_stub }, { 86, void_stub }, { 87, void_stub },
};

/*
 * JNI Fields
 */

// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
    { 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
    { 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
    { 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
    { 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
