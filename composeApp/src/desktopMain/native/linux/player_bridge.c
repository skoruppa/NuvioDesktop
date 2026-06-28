#define _GNU_SOURCE
#include <jni.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <math.h>
#include <locale.h>
#include <unistd.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <gtk/gtkx.h>
#include <webkit2/webkit2.h>
/* X11 backend — only included when GDK_IS_X11_WINDOW check succeeds at runtime */
#ifdef GDK_WINDOWING_X11
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
#  include <gdk/gdkx.h>
#endif
/* Wayland backend */
#ifdef GDK_WINDOWING_WAYLAND
#  include <gdk/gdkwayland.h>
#endif

static void *glGetEGLProcAddress(void *ctx, const char *name) {
    (void)ctx;
    return (void *)eglGetProcAddress(name);
}

/* ------------------------------------------------------------------ */
/*  Debug logging                                                      */
/* ------------------------------------------------------------------ */
#define DBG(...)  do { \
    fprintf(stderr, "[player_bridge] " __VA_ARGS__); \
    fflush(stderr); \
} while (0)

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
typedef struct PlayerInstance_s PlayerInstance;
static void webkit_script_message_cb(WebKitUserContentManager *ucm,
                                      WebKitJavascriptResult *result,
                                      gpointer userData);
static gboolean syncTimerCallback(gpointer userData);
static void buildPlayerUi(PlayerInstance *inst);

static int isWaylandSession() {
    const char *session = getenv("XDG_SESSION_TYPE");
    const char *display = getenv("WAYLAND_DISPLAY");
    return (session && strstr(session, "wayland")) || (display && display[0]);
}

/* mpv linked directly via -lmpv (no dlopen needed) */

/* ------------------------------------------------------------------ */
/*  Per-instance state                                                 */
/* ------------------------------------------------------------------ */
struct PlayerInstance_s {
    JavaVM *jvm;
    jobject eventSink;
    jmethodID eventMethod;
    mpv_handle *mpv;
    mpv_render_context *renderCtx;
    char *sourceUrl;
    char *controlsUrl;
    char **headers;
    int nheaders;
    volatile int alive;

    /* GTK thread */
    pthread_t gtkThread;

    /* GTK resources (valid while gtkThread runs) */
    GtkApplication *app;
    GMainLoop *mainLoop;
    GtkWidget *window;
    GtkWidget *drawingArea;
    GtkWidget *webView;
    GtkWidget *overlay;          /* GtkOverlay — supports auto-resize */

    /* Embed mode: when > 0, window embeds into host XID (AWT Canvas) */
    int embedMode;

    /* Offscreen mode: no window, mpv renders to offscreen buffer */
    int offscreenMode;

    /* EGL state for offscreen rendering */
    EGLDisplay eglDisplay;
    EGLContext eglContext;
    EGLSurface eglSurface;

    /* Offscreen FBO rendering */
    GLuint offscreenFbo;
    GLuint offscreenTexture;
    int offscreenWidth;
    int offscreenHeight;
    uint8_t *offscreenPixels;
    size_t offscreenPixelsSize;

    /* Session type */
    int isWayland;               /* 1 = Wayland, 0 = X11 */

    /* X11 */
#ifdef GDK_WINDOWING_X11
    Display *xDisplay;
    Window   xWindow;
    Window   hostXid;            /* Parent XID from AWT Canvas (embed mode only) */
#endif

    /* Sync controls timer */
    guint syncTimerId;

    /* Pending controls JSON */
    char *pendingControlsJson;
    int controlsReady;
};

/* ------------------------------------------------------------------ */
/*  JS escape helper                                                   */
/* ------------------------------------------------------------------ */
static char *escapeJsString(const char *input) {
    if (!input) return NULL;
    size_t len = strlen(input);
    size_t cap = len * 2 + 3;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; i++) {
        char c = input[i];
        if (c == '"')  { out[j++] = '\\'; out[j++] = '"'; }
        else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else { out[j++] = c; }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/*  Forward to Java event sink                                         */
/* ------------------------------------------------------------------ */
static void callEventSink(PlayerInstance *inst, const char *type, double value) {
    if (!inst->eventSink || !inst->eventMethod || !inst->jvm) return;
    JNIEnv *env = NULL;
    int detach = 0;
    jint ret = (*inst->jvm)->GetEnv(inst->jvm, (void**)&env, JNI_VERSION_1_6);
    if (ret == JNI_EDETACHED) {
        (*inst->jvm)->AttachCurrentThread(inst->jvm, (void**)&env, NULL);
        detach = 1;
    }
    if (env) {
        jstring jType = (*env)->NewStringUTF(env, type);
        (*env)->CallVoidMethod(env, inst->eventSink, inst->eventMethod, jType, value);
        (*env)->DeleteLocalRef(env, jType);
    }
    if (detach) (*inst->jvm)->DetachCurrentThread(inst->jvm);
}

/* ------------------------------------------------------------------ */
/*  WebKit JS bridge: receive messages from controls.html               */
/* ------------------------------------------------------------------ */
static void webkit_script_message_cb(WebKitUserContentManager *ucm,
                                      WebKitJavascriptResult *result,
                                      gpointer userData) {
    (void)ucm;
    PlayerInstance *inst = (PlayerInstance *)userData;
    if (!inst || !inst->alive) return;

    JSCValue *value = webkit_javascript_result_get_js_value(result);
    if (!value || !jsc_value_is_object(value)) return;

    JSCValue *typeVal = jsc_value_object_get_property(value, "type");
    JSCValue *numVal  = jsc_value_object_get_property(value, "value");
    if (!typeVal || !jsc_value_is_string(typeVal)) {
        if (typeVal) g_object_unref(typeVal);
        if (numVal) g_object_unref(numVal);
        return;
    }

    char *typeStr = jsc_value_to_string(typeVal);
    double dval = jsc_value_is_number(numVal) ? jsc_value_to_double(numVal) : 0.0;

    DBG("JS bridge: type=%s value=%.2f\n", typeStr, dval);

    callEventSink(inst, typeStr, dval);

    free(typeStr);
    g_object_unref(typeVal);
    g_object_unref(numVal);
    webkit_javascript_result_unref(result);
}

/* ------------------------------------------------------------------ */
/*  Sync timer: push state to controls.html every 500ms                */
/* ------------------------------------------------------------------ */
static gboolean syncTimerCallback(gpointer userData) {
    PlayerInstance *inst = (PlayerInstance *)userData;
    if (!inst || !inst->alive || !inst->mpv) return G_SOURCE_REMOVE;
    if (!inst->webView || !inst->controlsReady) return G_SOURCE_CONTINUE;

    double duration = 0, position = 0, cache = 0, speed = 1.0;
    int paused = 0, eof = 0, idle = 0, seeking = 0, buffering = 0;

    mpv_get_property(inst->mpv, "duration", MPV_FORMAT_DOUBLE, &duration);
    mpv_get_property(inst->mpv, "time-pos", MPV_FORMAT_DOUBLE, &position);
    mpv_get_property(inst->mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &cache);
    mpv_get_property(inst->mpv, "speed", MPV_FORMAT_DOUBLE, &speed);
    mpv_get_property(inst->mpv, "pause", MPV_FORMAT_FLAG, &paused);
    mpv_get_property(inst->mpv, "eof-reached", MPV_FORMAT_FLAG, &eof);
    mpv_get_property(inst->mpv, "core-idle", MPV_FORMAT_FLAG, &idle);
    mpv_get_property(inst->mpv, "seeking", MPV_FORMAT_FLAG, &seeking);
    mpv_get_property(inst->mpv, "paused-for-cache", MPV_FORMAT_FLAG, &buffering);

    int fileReady = duration > 0.0;
    int isLoading = !fileReady || (idle && !paused && !eof) || seeking || buffering;
    int isEnded = eof;
    int isPlaying = !paused && !isLoading && !isEnded;

    char snapshot[512];
    snprintf(snapshot, sizeof(snapshot),
        "{\"durationMs\":%lld,\"positionMs\":%lld,\"bufferedPositionMs\":%lld,"
        "\"isLoading\":%s,\"isPlaying\":%s,\"isEnded\":%s,\"isPaused\":%s,"
        "\"playbackSpeed\":%.2f}",
        (long long)(duration * 1000.0),
        (long long)(position * 1000.0),
        (long long)((position + cache) * 1000.0),
        isLoading ? "true" : "false",
        isPlaying ? "true" : "false",
        isEnded ? "true" : "false",
        paused ? "true" : "false",
        speed);

    char *escaped = escapeJsString(snapshot);
    if (!escaped) return G_SOURCE_CONTINUE;

    char script[1024];
    snprintf(script, sizeof(script),
        "(function(){if(!window.playerControls)return;try{window.playerControls(JSON.parse(%s));}catch(e){}})()",
        escaped);
    free(escaped);

    webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(inst->webView), script, -1, NULL, NULL, NULL, NULL, NULL);

    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/*  WebKit load-changed callback (detect controlsReady)                 */
/* ------------------------------------------------------------------ */
static void webKitLoadChanged(WebKitWebView *webView, WebKitLoadEvent event, gpointer userData) {
    (void)webView;
    PlayerInstance *inst = (PlayerInstance *)userData;
    if (event == WEBKIT_LOAD_FINISHED) {
        inst->controlsReady = 1;
        DBG("Controls page loaded\n");
        /* Send any pending controls JSON */
        if (inst->pendingControlsJson) {
            char *escaped = escapeJsString(inst->pendingControlsJson);
            if (escaped) {
                char script[4096];
                snprintf(script, sizeof(script),
                    "(function(){if(!window.playerControls)return;"
                    "window.playerControls(JSON.parse(%s));})()",
                    escaped);
                webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(inst->webView), script, -1, NULL, NULL, NULL, NULL, NULL);
                free(escaped);
            }
            free(inst->pendingControlsJson);
            inst->pendingControlsJson = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Forward declaration                                                */
/* ------------------------------------------------------------------ */
static void start_mpv_after_realize(PlayerInstance *inst);

/* ------------------------------------------------------------------ */
/*  realize callback: يُشغَّل بعد أن يصبح X11 window جاهزاً فعلاً    */
/*  هذا هو الإصلاح الجوهري لمشكلة النوافذ الثلاث:                    */
/*  mpv يحتاج XID صالح — لا يتوفر إلا بعد realize، ليس بعد show_all */
/* ------------------------------------------------------------------ */
static void on_drawing_area_realize(GtkWidget *widget, gpointer userData) {
    PlayerInstance *inst = (PlayerInstance *)userData;
    if (!inst || !inst->alive) return;

    GdkWindow *gdkWin = gtk_widget_get_window(widget);
    if (!gdkWin) {
        DBG("realize: gdkWin is NULL\n");
        return;
    }

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_WINDOW(gdkWin)) {
        inst->isWayland = 0;
        inst->xDisplay  = GDK_DISPLAY_XDISPLAY(gtk_widget_get_display(widget));
        inst->xWindow   = GDK_WINDOW_XID(gdkWin);
        DBG("realize: X11 XID=%lu display=%p\n",
            (unsigned long)inst->xWindow, (void *)inst->xDisplay);
    } else
#endif
#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_WINDOW(gdkWin)) {
        inst->isWayland = 1;
        DBG("realize: Wayland session\n");
    } else
#endif
    {
        inst->isWayland = 0;
        DBG("realize: unknown GDK backend, fallback to X11 path\n");
    }

    /* الآن XID مضمون الصحة — نُهيِّئ mpv */
    start_mpv_after_realize(inst);
}

/* ------------------------------------------------------------------ */
/*  start_mpv_after_realize: إنشاء وتهيئة mpv بعد realize             */
/* ------------------------------------------------------------------ */
static void start_mpv_after_realize(PlayerInstance *inst) {
    setlocale(LC_NUMERIC, "C");

    mpv_handle *mpv = mpv_create();
    if (!mpv) {
        DBG("mpv_create failed\n");
        return;
    }
    inst->mpv = mpv;

    /* خيارات mpv */
    mpv_set_option_string(mpv, "config",                  "no");
    mpv_set_option_string(mpv, "osc",                     "no");
    mpv_set_option_string(mpv, "input-default-bindings",  "yes");
    mpv_set_option_string(mpv, "input-vo-keyboard",       "no");
    mpv_set_option_string(mpv, "keep-open",               "yes");
    mpv_set_option_string(mpv, "hwdec",                   "auto");
    mpv_set_option_string(mpv, "hwdec-codecs",            "all");
    mpv_set_option_string(mpv, "vd-lavc-dr",              "yes");
    mpv_set_option_string(mpv, "video-latency-hacks",     "yes");
    mpv_set_option_string(mpv, "cache",                   "yes");
    mpv_set_option_string(mpv, "cache-secs",              "3");
    mpv_set_option_string(mpv, "demuxer-max-bytes",       "50M");
    mpv_set_option_string(mpv, "demuxer-max-back-bytes",  "25M");
    mpv_set_option_string(mpv, "audio-file-auto",         "no");
    mpv_set_option_string(mpv, "sub-auto",                "no");
    mpv_set_option_string(mpv, "terminal",                "no");
    mpv_set_option_string(mpv, "msg-level",               "all=status");

#ifdef GDK_WINDOWING_X11
    if (!inst->isWayland && inst->xWindow) {
        /* *** الإصلاح الجوهري: XID صالح من realize — mpv يعرض داخل GTK *** */
        char widStr[64];
        snprintf(widStr, sizeof(widStr), "%lu", (unsigned long)inst->xWindow);
        mpv_set_option_string(mpv, "wid",     widStr);
        mpv_set_option_string(mpv, "vo",      "gpu");
        mpv_set_option_string(mpv, "gpu-api", "opengl");
        DBG("start_mpv: X11 wid=%s\n", widStr);
    } else
#endif
    {
        mpv_set_option_string(mpv, "vo",          "gpu");
        mpv_set_option_string(mpv, "gpu-api",     "opengl");
        mpv_set_option_string(mpv, "gpu-context", "wayland");
        DBG("start_mpv: Wayland mode\n");
    }

    /* HTTP headers */
    if (inst->nheaders > 0 && inst->headers) {
        size_t len = 0;
        for (int i = 0; i < inst->nheaders; i++)
            if (inst->headers[i]) len += strlen(inst->headers[i]) * 2 + 2;
        if (len > 0) {
            char *hdr = malloc(len + 1);
            hdr[0] = '\0';
            for (int i = 0; i < inst->nheaders; i++) {
                if (!inst->headers[i]) continue;
                if (hdr[0] != '\0') strcat(hdr, ",");
                const char *src = inst->headers[i];
                char *dst = hdr + strlen(hdr);
                while (*src) {
                    if (*src == '\\' || *src == ',') *dst++ = '\\';
                    *dst++ = *src++;
                }
                *dst = '\0';
            }
            mpv_set_option_string(mpv, "http-header-fields", hdr);
            free(hdr);
        }
    }

    /* تهيئة mpv */
    if (mpv_initialize(mpv) < 0) {
        DBG("start_mpv: mpv_initialize failed\n");
        mpv_terminate_destroy(mpv);
        inst->mpv = NULL;
        return;
    }

    /* تشغيل الفيديو */
    if (inst->sourceUrl && inst->sourceUrl[0]) {
        const char *cmd[] = {"loadfile", inst->sourceUrl, "replace", NULL};
        mpv_command(mpv, cmd);
        DBG("start_mpv: loadfile %s\n", inst->sourceUrl);
    }

    /* مؤقت المزامنة كل 500ms */
    inst->syncTimerId = g_timeout_add(500, syncTimerCallback, inst);
}

/* ------------------------------------------------------------------ */
/*  Standalone setup: GtkApplication with DrawingArea + WebKitGTK       */
/* ------------------------------------------------------------------ */
static void gtkAppActivate(GApplication *app, gpointer userData) {
    PlayerInstance *inst = (PlayerInstance *)userData;
    (void)app;

    /* Ensure C locale for mpv (it rejects non-C LC_NUMERIC) */
    setlocale(LC_NUMERIC, "C");

    /* Create standalone window */
    inst->window = gtk_application_window_new(inst->app);
    gtk_window_set_title(GTK_WINDOW(inst->window), "Nuvio Player");
    gtk_window_set_default_size(GTK_WINDOW(inst->window), 1280, 720);
    DBG("Standalone GtkWindow created\n");

    buildPlayerUi(inst);
}

/* ------------------------------------------------------------------ */
/*  Helper: build the player UI (overlay + drawing area + webview)     */
/* ------------------------------------------------------------------ */
static void addWebViewOverlay(PlayerInstance *inst) {
    if (!inst->controlsUrl || !inst->controlsUrl[0]) return;

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    inst->webView = webkit_web_view_new_with_user_content_manager(ucm);

    /* Transparent background so video shows through */
    webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(inst->webView),
        &(GdkRGBA){0, 0, 0, 0});

    gtk_widget_set_name(inst->webView, "webView");
    gtk_widget_set_hexpand(inst->webView, TRUE);
    gtk_widget_set_vexpand(inst->webView, TRUE);
    gtk_widget_set_can_focus(inst->webView, TRUE);
    gtk_widget_set_sensitive(inst->webView, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(inst->overlay), inst->webView);

    /* JS bridge: receive messages from controls.html */
    webkit_user_content_manager_register_script_message_handler(ucm, "player");
    g_signal_connect(ucm, "script-message-received::player",
                     G_CALLBACK(webkit_script_message_cb), inst);

    g_signal_connect(inst->webView, "load-changed",
                     G_CALLBACK(webKitLoadChanged), inst);

    DBG("Loading controls: %s\n", inst->controlsUrl);
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(inst->webView), inst->controlsUrl);
}

static void buildPlayerUi(PlayerInstance *inst) {
    /* Black background via CSS (GTK3 API) */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background-color: black; }"
        "#drawingArea { background-color: black; }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /*
     * GtkOverlay: child (drawingArea) fills the window,
     * overlay child (webView) is stacked on top and also fills.
     */
    inst->overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(inst->window), inst->overlay);

    /* Drawing area for mpv rendering — the "base" child of GtkOverlay */
    inst->drawingArea = gtk_drawing_area_new();
    gtk_widget_set_name(inst->drawingArea, "drawingArea");
    gtk_widget_set_hexpand(inst->drawingArea, TRUE);
    gtk_widget_set_vexpand(inst->drawingArea, TRUE);
    gtk_widget_set_has_window(inst->drawingArea, TRUE);
    gtk_container_add(GTK_CONTAINER(inst->overlay), inst->drawingArea);

    /*
     * Register realize callback before show_all.
     * realize fires after show_all when the X11 window actually exists.
     */
    g_signal_connect(inst->drawingArea, "realize",
                     G_CALLBACK(on_drawing_area_realize), inst);

    /* WebKitWebView overlay for controls.html — only in embed mode */
    if (inst->embedMode) {
        addWebViewOverlay(inst);
    } else {
        DBG("Standalone mode: no webview overlay, using Compose controls\n");
    }

    gtk_widget_show_all(inst->window);
}

/* ------------------------------------------------------------------ */
/*  GTK main loop thread                                               */
/* ------------------------------------------------------------------ */
static void *gtkThreadFunc(void *data) {
    PlayerInstance *inst = (PlayerInstance *)data;

    gtk_init(NULL, NULL);

    if (inst->embedMode) {
#ifdef GDK_WINDOWING_X11
        /* Embedded: GtkPlug inside AWT Canvas, no GtkApplication needed */
        inst->window = gtk_plug_new(inst->hostXid);
        DBG("Embedded: GtkPlug created, hostXid=0x%lx\n", (unsigned long)inst->hostXid);
        buildPlayerUi(inst);
        inst->mainLoop = g_main_loop_new(NULL, FALSE);
        DBG("Entering embedded GTK main loop\n");
        g_main_loop_run(inst->mainLoop);
        DBG("Embedded GTK main loop exited\n");
#else
        DBG("Embed mode not supported on non-X11 GDK backend\n");
#endif
    } else {
        /* Standalone: GtkApplication with its own window */
        inst->app = gtk_application_new("com.nuvio.player", G_APPLICATION_DEFAULT_FLAGS);
        g_signal_connect(inst->app, "activate", G_CALLBACK(gtkAppActivate), inst);
        DBG("Entering standalone GTK main loop\n");
        g_application_run(G_APPLICATION(inst->app), 0, NULL);
        DBG("Standalone GTK main loop exited\n");
    }

    /* Cleanup */
    if (inst->syncTimerId) {
        g_source_remove(inst->syncTimerId);
        inst->syncTimerId = 0;
    }
    if (inst->renderCtx) {
        mpv_render_context_free(inst->renderCtx);
        inst->renderCtx = NULL;
    }
    if (inst->mpv) {
        mpv_terminate_destroy(inst->mpv);
        inst->mpv = NULL;
    }

    /* Free resources */
    if (inst->sourceUrl) { free(inst->sourceUrl); inst->sourceUrl = NULL; }
    if (inst->controlsUrl) { free(inst->controlsUrl); inst->controlsUrl = NULL; }
    if (inst->headers) {
        for (int i = 0; i < inst->nheaders; i++) free(inst->headers[i]);
        free(inst->headers);
        inst->headers = NULL;
    }
    if (inst->pendingControlsJson) { free(inst->pendingControlsJson); inst->pendingControlsJson = NULL; }

    if (inst->mainLoop) {
        g_main_loop_unref(inst->mainLoop);
        inst->mainLoop = NULL;
    }
    if (inst->app) {
        g_object_unref(inst->app);
        inst->app = NULL;
    }
    inst->window = NULL;
    inst->drawingArea = NULL;
    inst->webView = NULL;

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Offscreen: EGL + libmpv rendering                                   */
/* ------------------------------------------------------------------ */
static int initOffscreenEGL(PlayerInstance *inst) {
    inst->eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (inst->eglDisplay == EGL_NO_DISPLAY) {
        DBG("offscreen: eglGetDisplay failed\n");
        return 0;
    }

    EGLint major, minor;
    if (!eglInitialize(inst->eglDisplay, &major, &minor)) {
        DBG("offscreen: eglInitialize failed\n");
        return 0;
    }

    EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(inst->eglDisplay, configAttribs, &config, 1, &numConfigs)) {
        DBG("offscreen: eglChooseConfig failed\n");
        return 0;
    }

    EGLint pbufferAttribs[] = {
        EGL_WIDTH, 1, EGL_HEIGHT, 1,
        EGL_NONE
    };
    inst->eglSurface = eglCreatePbufferSurface(inst->eglDisplay, config, pbufferAttribs);
    if (inst->eglSurface == EGL_NO_SURFACE) {
        DBG("offscreen: eglCreatePbufferSurface failed\n");
        return 0;
    }

    eglBindAPI(EGL_OPENGL_API);

    EGLint ctxAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE
    };
    inst->eglContext = eglCreateContext(inst->eglDisplay, config, EGL_NO_CONTEXT, ctxAttribs);
    if (inst->eglContext == EGL_NO_CONTEXT) {
        DBG("offscreen: eglCreateContext failed\n");
        return 0;
    }

    if (!eglMakeCurrent(inst->eglDisplay, inst->eglSurface, inst->eglSurface, inst->eglContext)) {
        DBG("offscreen: eglMakeCurrent failed\n");
        return 0;
    }

    /* Create FBO for offscreen rendering */
    inst->offscreenFbo = 0;
    inst->offscreenTexture = 0;
    inst->offscreenWidth = 0;
    inst->offscreenHeight = 0;
    inst->offscreenPixels = NULL;
    inst->offscreenPixelsSize = 0;

    DBG("offscreen: EGL initialized successfully\n");
    return 1;
}

static void fboResize(PlayerInstance *inst, int w, int h) {
    if (inst->offscreenWidth == w && inst->offscreenHeight == h)
        return;

    if (inst->offscreenFbo) {
        glDeleteFramebuffers(1, &inst->offscreenFbo);
        glDeleteTextures(1, &inst->offscreenTexture);
    }

    glGenTextures(1, &inst->offscreenTexture);
    glBindTexture(GL_TEXTURE_2D, inst->offscreenTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &inst->offscreenFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, inst->offscreenFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, inst->offscreenTexture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    inst->offscreenWidth = w;
    inst->offscreenHeight = h;

    size_t needed = (size_t)w * h * 4;
    if (needed > inst->offscreenPixelsSize) {
        free(inst->offscreenPixels);
        inst->offscreenPixels = malloc(needed);
        inst->offscreenPixelsSize = needed;
    }

    DBG("offscreen: FBO resized to %dx%d\n", w, h);
}

static void *offscreenThreadFunc(void *data) {
    PlayerInstance *inst = (PlayerInstance *)data;

    setlocale(LC_NUMERIC, "C");

    mpv_handle *mpv = mpv_create();
    if (!mpv) {
        DBG("offscreen: mpv_create failed\n");
        return NULL;
    }
    inst->mpv = mpv;

    /* خيارات mpv */
    mpv_set_option_string(mpv, "config",                  "no");
    mpv_set_option_string(mpv, "osc",                     "no");
    mpv_set_option_string(mpv, "input-default-bindings",  "yes");
    mpv_set_option_string(mpv, "input-vo-keyboard",       "no");
    mpv_set_option_string(mpv, "keep-open",               "yes");
    mpv_set_option_string(mpv, "hwdec",                   "auto");
    mpv_set_option_string(mpv, "hwdec-codecs",            "all");
    mpv_set_option_string(mpv, "vd-lavc-dr",              "yes");
    mpv_set_option_string(mpv, "video-latency-hacks",     "yes");
    mpv_set_option_string(mpv, "cache",                   "yes");
    mpv_set_option_string(mpv, "cache-secs",              "3");
    mpv_set_option_string(mpv, "demuxer-max-bytes",       "50M");
    mpv_set_option_string(mpv, "demuxer-max-back-bytes",  "25M");
    mpv_set_option_string(mpv, "audio-file-auto",         "no");
    mpv_set_option_string(mpv, "sub-auto",                "no");
    mpv_set_option_string(mpv, "terminal",                "no");
    mpv_set_option_string(mpv, "msg-level",               "all=status");

    /* Offscreen mode: vo=libmpv */
    mpv_set_option_string(mpv, "vo",          "libmpv");
    mpv_set_option_string(mpv, "gpu-api",     "opengl");
    mpv_set_option_string(mpv, "gpu-context", "wayland");

    /* تهيئة EGL */
    if (!initOffscreenEGL(inst)) {
        mpv_terminate_destroy(mpv);
        inst->mpv = NULL;
        return NULL;
    }

    /* HTTP headers */
    if (inst->nheaders > 0 && inst->headers) {
        size_t len = 0;
        for (int i = 0; i < inst->nheaders; i++)
            if (inst->headers[i]) len += strlen(inst->headers[i]) * 2 + 2;
        if (len > 0) {
            char *hdr = malloc(len + 1);
            hdr[0] = '\0';
            for (int i = 0; i < inst->nheaders; i++) {
                if (!inst->headers[i]) continue;
                if (hdr[0] != '\0') strcat(hdr, ",");
                const char *src = inst->headers[i];
                char *dst = hdr + strlen(hdr);
                while (*src) {
                    if (*src == '\\' || *src == ',') *dst++ = '\\';
                    *dst++ = *src++;
                }
                *dst = '\0';
            }
            mpv_set_option_string(mpv, "http-header-fields", hdr);
            free(hdr);
        }
    }

    /* تهيئة mpv */
    if (mpv_initialize(mpv) < 0) {
        DBG("offscreen: mpv_initialize failed\n");
        mpv_terminate_destroy(mpv);
        inst->mpv = NULL;
        return NULL;
    }

    /* إنشاء render context */
    mpv_opengl_init_params glInitParams = {glGetEGLProcAddress, NULL};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInitParams},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    if (mpv_render_context_create(&inst->renderCtx, mpv, params) < 0) {
        DBG("offscreen: mpv_render_context_create failed\n");
        mpv_terminate_destroy(mpv);
        inst->mpv = NULL;
        return NULL;
    }

    DBG("offscreen: mpv render context created\n");

    /* تشغيل الفيديو */
    if (inst->sourceUrl && inst->sourceUrl[0]) {
        const char *cmd[] = {"loadfile", inst->sourceUrl, "replace", NULL};
        mpv_command(mpv, cmd);
        DBG("offscreen: loadfile %s\n", inst->sourceUrl);
    }

    /* Event loop */
    while (inst->alive) {
        mpv_event *event = mpv_wait_event(mpv, 0.5);
        if (event == NULL) continue;
        if (event->event_id == MPV_EVENT_SHUTDOWN) break;
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            DBG("offscreen: playback started\n");
        }
    }

    DBG("offscreen: event loop exited\n");

    /* تنظيف */
    if (inst->offscreenFbo) {
        glDeleteFramebuffers(1, &inst->offscreenFbo);
        glDeleteTextures(1, &inst->offscreenTexture);
    }
    free(inst->offscreenPixels);
    inst->offscreenPixels = NULL;

    if (inst->renderCtx) {
        mpv_render_context_free(inst->renderCtx);
        inst->renderCtx = NULL;
    }
    if (inst->mpv) {
        mpv_terminate_destroy(inst->mpv);
        inst->mpv = NULL;
    }
    if (inst->eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(inst->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (inst->eglContext != EGL_NO_CONTEXT) eglDestroyContext(inst->eglDisplay, inst->eglContext);
        if (inst->eglSurface != EGL_NO_SURFACE) eglDestroySurface(inst->eglDisplay, inst->eglSurface);
        eglTerminate(inst->eglDisplay);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  create                                                             */
/* ------------------------------------------------------------------ */
JNIEXPORT jlong JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_create(
    JNIEnv *env, jobject thiz,
    jlong hostViewPtr,
    jint hostWidth, jint hostHeight,
    jstring sourceUrl,
    jobjectArray headerLines, jboolean playWhenReady, jlong initialPositionMs,
    jstring controlsPageUrl,
    jint decoderPriority, jboolean nvidiaRtxSuperResolutionEnabled,
    jobject eventSink) {
    (void)thiz;
    (void)hostWidth; (void)hostHeight;
    (void)playWhenReady; (void)initialPositionMs;
    (void)decoderPriority; (void)nvidiaRtxSuperResolutionEnabled;

    DBG("create() called (GTK/WebKitGTK mode) hostViewPtr=0x%lx\n", (unsigned long)hostViewPtr);

    PlayerInstance *inst = calloc(1, sizeof(PlayerInstance));
    if (!inst) return 0;

    (*env)->GetJavaVM(env, &inst->jvm);
    inst->alive = 1;

    /* Detect embed mode: when hostViewPtr != 0, it's an XID from AWT Canvas */
    if (hostViewPtr != 0) {
        inst->embedMode = 1;
        inst->hostXid = (Window)hostViewPtr;
        DBG("Embed mode: hostXid=0x%lx\n", (unsigned long)inst->hostXid);
    }

    /* Capture event sink */
    if (eventSink) {
        inst->eventSink = (*env)->NewGlobalRef(env, eventSink);
        jclass cls = (*env)->GetObjectClass(env, eventSink);
        inst->eventMethod = (*env)->GetMethodID(env, cls, "onPlayerEvent", "(Ljava/lang/String;D)V");
        (*env)->DeleteLocalRef(env, cls);
    }

    /* Copy source URL */
    if (sourceUrl) {
        const char *src = (*env)->GetStringUTFChars(env, sourceUrl, NULL);
        if (src) { inst->sourceUrl = strdup(src); (*env)->ReleaseStringUTFChars(env, sourceUrl, src); }
    }

    /* Copy controls URL */
    if (controlsPageUrl) {
        const char *url = (*env)->GetStringUTFChars(env, controlsPageUrl, NULL);
        if (url && url[0]) {
            inst->controlsUrl = strdup(url);
            DBG("Controls URL: %s\n", url);
        }
        if (url) (*env)->ReleaseStringUTFChars(env, controlsPageUrl, url);
    }

    /* Copy headers */
    jsize nh = headerLines ? (*env)->GetArrayLength(env, headerLines) : 0;
    inst->nheaders = nh;
    if (nh > 0) {
        inst->headers = calloc(nh, sizeof(char *));
        for (jsize i = 0; i < nh; i++) {
            jstring s = (jstring)(*env)->GetObjectArrayElement(env, headerLines, i);
            const char *h = (*env)->GetStringUTFChars(env, s, NULL);
            if (h) { inst->headers[i] = strdup(h); (*env)->ReleaseStringUTFChars(env, s, h); }
            (*env)->DeleteLocalRef(env, s);
        }
    }

    /* Start GTK thread or offscreen thread */
    if (hostViewPtr == 0L && isWaylandSession()) {
        inst->offscreenMode = 1;
        DBG("Offscreen mode: no window, libmpv rendering\n");
        pthread_create(&inst->gtkThread, NULL, offscreenThreadFunc, inst);
    } else {
        /* Start GTK thread (creates window, mpv, webview, runs main loop) */
        DBG("GTK mode: creating window\n");
        pthread_create(&inst->gtkThread, NULL, gtkThreadFunc, inst);
    }

    DBG("create() complete, handle=%p\n", (void*)inst);
    return (jlong)(intptr_t)inst;
}

/* ------------------------------------------------------------------ */
/*  dispose                                                            */
/* ------------------------------------------------------------------ */
JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_dispose(
    JNIEnv *env, jobject thiz, jlong handle) {
    (void)thiz;
    if (!handle) return;
    PlayerInstance *inst = (PlayerInstance *)(intptr_t)handle;
    DBG("dispose() called\n");

    inst->alive = 0;

    /* Quit GTK main loop */
    if (inst->embedMode && inst->mainLoop) {
        GMainContext *ctx = g_main_context_default();
        g_main_context_invoke(ctx, (GSourceFunc)g_main_loop_quit, inst->mainLoop);
    } else if (inst->app) {
        GMainContext *ctx = g_main_context_default();
        g_main_context_invoke(ctx, (GSourceFunc)g_application_quit, inst->app);
    }

    /* Wait for GTK thread to finish */
    if (inst->gtkThread) {
        pthread_join(inst->gtkThread, NULL);
    }

    /* Release event sink */
    if (inst->eventSink && inst->jvm) {
        JNIEnv *e = NULL;
        int detach = 0;
        jint ret = (*inst->jvm)->GetEnv(inst->jvm, (void**)&e, JNI_VERSION_1_6);
        if (ret == JNI_EDETACHED) {
            (*inst->jvm)->AttachCurrentThread(inst->jvm, (void**)&e, NULL);
            detach = 1;
        }
        if (e) (*e)->DeleteGlobalRef(e, inst->eventSink);
        if (detach) (*inst->jvm)->DetachCurrentThread(inst->jvm);
    }

    free(inst);
}

/* ------------------------------------------------------------------ */
/*  updateControls — push JSON to controls.html                        */
/* ------------------------------------------------------------------ */
JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_updateControls(
    JNIEnv *env, jobject thiz, jlong handle, jstring json) {
    (void)thiz;
    if (!handle || !json) return;
    PlayerInstance *inst = (PlayerInstance *)(intptr_t)handle;
    if (!inst->webView) return;

    const char *jsonStr = (*env)->GetStringUTFChars(env, json, NULL);
    if (!jsonStr) return;

    if (!inst->controlsReady) {
        /* Page not loaded yet — cache the JSON */
        free(inst->pendingControlsJson);
        inst->pendingControlsJson = strdup(jsonStr);
        (*env)->ReleaseStringUTFChars(env, json, jsonStr);
        return;
    }

    char *escaped = escapeJsString(jsonStr);
    (*env)->ReleaseStringUTFChars(env, json, jsonStr);
    if (!escaped) return;

    char script[4096];
    snprintf(script, sizeof(script),
        "(function(){if(!window.playerControls)return;"
        "window.playerControls(JSON.parse(%s));})()",
        escaped);
    free(escaped);

    webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(inst->webView), script, -1, NULL, NULL, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  requestFocus                                                       */
/* ------------------------------------------------------------------ */
JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_requestFocus(
    JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    if (!handle) return;
    PlayerInstance *inst = (PlayerInstance *)(intptr_t)handle;
    if (inst->webView) {
        gtk_widget_grab_focus(inst->webView);
    }
}

/* ------------------------------------------------------------------ */
/*  resizeNativeView                                                   */
/* ------------------------------------------------------------------ */
JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_resizeNativeView(
    JNIEnv *env, jobject thiz, jlong h, jint w, jint ht) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->window) return;

    gtk_window_resize(GTK_WINDOW(inst->window), w, ht);

    if (inst->mpv && !inst->isWayland) {
        mpv_set_property_string(inst->mpv, "video-unscaled", "no");
    }
}

/* ------------------------------------------------------------------ */
/*  Playback controls                                                  */
/* ------------------------------------------------------------------ */
JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setPaused(
    JNIEnv *env, jobject thiz, jlong h, jboolean paused) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    mpv_set_property_string(inst->mpv, "pause", paused ? "yes" : "no");
}

JNIEXPORT jboolean JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isPaused(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return JNI_TRUE;
    int v = 0;
    mpv_get_property(inst->mpv, "pause", MPV_FORMAT_FLAG, &v);
    return v ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekTo(
    JNIEnv *env, jobject thiz, jlong h, jlong posMs) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%f", (double)posMs / 1000.0);
    const char *cmd[] = {"seek", buf, "absolute+keyframes", NULL};
    mpv_command(inst->mpv, cmd);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekBy(
    JNIEnv *env, jobject thiz, jlong h, jlong offMs) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", (long)(offMs / 1000));
    const char *cmd[] = {"seek", buf, "relative", NULL};
    mpv_command(inst->mpv, cmd);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSpeed(
    JNIEnv *env, jobject thiz, jlong h, jfloat speed) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%f", (double)speed);
    mpv_set_property_string(inst->mpv, "speed", buf);
}

JNIEXPORT jfloat JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_speed(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return 1.0f;
    double v = 1.0;
    mpv_get_property(inst->mpv, "speed", MPV_FORMAT_DOUBLE, &v);
    return (jfloat)v;
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setVolume(
    JNIEnv *env, jobject thiz, jlong h, jfloat level) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    double vol = (double)level * 100.0;
    if (vol < 0) vol = 0; if (vol > 200) vol = 200;
    char buf[32];
    snprintf(buf, sizeof(buf), "%f", vol);
    mpv_set_property_string(inst->mpv, "volume", buf);
}

JNIEXPORT jfloat JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_volume(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return 0.0f;
    double vol = 100.0;
    mpv_get_property(inst->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    return (jfloat)(vol / 100.0);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_adjustVolume(
    JNIEnv *env, jobject thiz, jlong h, jfloat delta) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    double vol = 100.0;
    mpv_get_property(inst->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    vol += (double)delta;
    if (vol < 0) vol = 0; if (vol > 200) vol = 200;
    char buf[32];
    snprintf(buf, sizeof(buf), "%f", vol);
    mpv_set_property_string(inst->mpv, "volume", buf);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setResizeMode(
    JNIEnv *env, jobject thiz, jlong h, jint mode) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    switch (mode) {
        case 1: case 2:
            mpv_set_property_string(inst->mpv, "keepaspect", "yes");
            mpv_set_property_string(inst->mpv, "panscan", "1.0");
            mpv_set_property_string(inst->mpv, "video-unscaled", "no");
            break;
        case 3:
            mpv_set_property_string(inst->mpv, "keepaspect", "no");
            mpv_set_property_string(inst->mpv, "panscan", "0.0");
            mpv_set_property_string(inst->mpv, "video-unscaled", "no");
            break;
        default:
            mpv_set_property_string(inst->mpv, "keepaspect", "yes");
            mpv_set_property_string(inst->mpv, "panscan", "0.0");
            mpv_set_property_string(inst->mpv, "video-unscaled", "no");
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Property getters                                                   */
/* ------------------------------------------------------------------ */
JNIEXPORT jlong JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_durationMs(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return 0;
    double d = 0;
    mpv_get_property(inst->mpv, "duration", MPV_FORMAT_DOUBLE, &d);
    return (jlong)(d * 1000.0);
}

JNIEXPORT jlong JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_positionMs(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return 0;
    double p = 0;
    mpv_get_property(inst->mpv, "time-pos", MPV_FORMAT_DOUBLE, &p);
    return (jlong)(p * 1000.0);
}

JNIEXPORT jlong JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_bufferedPositionMs(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return 0;
    double pos = 0, cache = 0;
    mpv_get_property(inst->mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    mpv_get_property(inst->mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &cache);
    return (jlong)((pos + cache) * 1000.0);
}

JNIEXPORT jboolean JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isLoading(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return JNI_FALSE;
    int v = 0;
    mpv_get_property(inst->mpv, "paused-for-cache", MPV_FORMAT_FLAG, &v);
    if (v) return JNI_TRUE;
    mpv_get_property(inst->mpv, "seeking", MPV_FORMAT_FLAG, &v);
    return v ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isEnded(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return JNI_FALSE;
    int v = 0;
    mpv_get_property(inst->mpv, "eof-reached", MPV_FORMAT_FLAG, &v);
    return v ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_videoWidth(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return 0;
    int64_t w = 0;
    mpv_get_property(inst->mpv, "dwidth", MPV_FORMAT_INT64, &w);
    return (jint)w;
}

JNIEXPORT jint JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_videoHeight(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return 0;
    int64_t ht = 0;
    mpv_get_property(inst->mpv, "dheight", MPV_FORMAT_INT64, &ht);
    return (jint)ht;
}

/* ------------------------------------------------------------------ */
/*  Track management                                                   */
/* ------------------------------------------------------------------ */
static char *tracks_json_for_type(mpv_handle *mpv, const char *type) {
    mpv_node tracks;
    if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &tracks) < 0)
        return strdup("[]");

    size_t cap = 1024;
    char *buf = malloc(cap);
    size_t pos = 0;
    #define ENSURE(n) while (pos + (n) + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
    #define APPEND(s) do { const char *_s = (s); size_t _l = strlen(_s); ENSURE(_l) memcpy(buf+pos,_s,_l); pos+=_l; buf[pos]='\0'; } while(0)
    #define CH(c) do { ENSURE(1) buf[pos++]=(c); buf[pos]='\0'; } while(0)

    CH('[');
    if (tracks.format == MPV_FORMAT_NODE_ARRAY && tracks.u.list) {
        int first = 1;
        for (int i = 0; i < tracks.u.list->num; i++) {
            mpv_node *node = &tracks.u.list->values[i];
            if (node->format != MPV_FORMAT_NODE_MAP || !node->u.list) continue;
            const char *node_type = NULL;
            int track_id = 0, selected = 0;
            const char *lang = NULL, *label = NULL;
            for (int j = 0; j < node->u.list->num; j++) {
                const char *key = node->u.list->keys[j];
                mpv_node *val = &node->u.list->values[j];
                if (strcmp(key, "type") == 0 && val->format == MPV_FORMAT_STRING) node_type = val->u.string;
                else if (strcmp(key, "id") == 0 && val->format == MPV_FORMAT_INT64) track_id = (int)val->u.int64;
                else if (strcmp(key, "selected") == 0 && val->format == MPV_FORMAT_FLAG) selected = val->u.flag;
                else if (strcmp(key, "lang") == 0 && val->format == MPV_FORMAT_STRING) lang = val->u.string;
                else if (strcmp(key, "title") == 0 && val->format == MPV_FORMAT_STRING) label = val->u.string;
            }
            if (!node_type || strcmp(node_type, type) != 0) continue;
            if (!first) CH(',');
            first = 0;
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "{\"id\":\"%d\",\"index\":%d,\"label\":\"", track_id, track_id - 1);
            APPEND(tmp);
            const char *lbl = label ? label : (lang ? lang : "Track");
            for (const char *p = lbl; *p; p++) {
                if (*p == '"') APPEND("\\\"");
                else if (*p == '\\') APPEND("\\\\");
                else { ENSURE(1) buf[pos++] = *p; buf[pos] = '\0'; }
            }
            snprintf(tmp, sizeof(tmp), "\",\"language\":\"");
            APPEND(tmp);
            const char *lg = lang ? lang : "";
            for (const char *p = lg; *p; p++) {
                if (*p == '"') APPEND("\\\"");
                else if (*p == '\\') APPEND("\\\\");
                else { ENSURE(1) buf[pos++] = *p; buf[pos] = '\0'; }
            }
            snprintf(tmp, sizeof(tmp), "\",\"selected\":%s}", selected ? "true" : "false");
            APPEND(tmp);
        }
    }
    CH(']');
    #undef ENSURE
    #undef APPEND
    #undef CH
    mpv_free_node_contents(&tracks);
    return buf ? buf : strdup("[]");
}

JNIEXPORT jstring JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_audioTracksJson(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return (*env)->NewStringUTF(env, "[]");
    char *json = tracks_json_for_type(inst->mpv, "audio");
    jstring r = (*env)->NewStringUTF(env, json ? json : "[]");
    free(json);
    return r;
}

JNIEXPORT jstring JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_subtitleTracksJson(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return (*env)->NewStringUTF(env, "[]");
    char *json = tracks_json_for_type(inst->mpv, "sub");
    jstring r = (*env)->NewStringUTF(env, json ? json : "[]");
    free(json);
    return r;
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectAudioTrack(
    JNIEnv *env, jobject thiz, jlong h, jint id) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", id);
    mpv_set_property_string(inst->mpv, "aid", buf);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectSubtitleTrack(
    JNIEnv *env, jobject thiz, jlong h, jint id) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    if (id <= 0) { mpv_set_property_string(inst->mpv, "sid", "no"); return; }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", id);
    mpv_set_property_string(inst->mpv, "sid", buf);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_addSubtitleUrl(
    JNIEnv *env, jobject thiz, jlong h, jstring url) {
    (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv || !url) return;
    const char *u = (*env)->GetStringUTFChars(env, url, NULL);
    if (u) {
        const char *cmd[] = {"sub-add", u, "auto", NULL};
        mpv_command(inst->mpv, cmd);
        (*env)->ReleaseStringUTFChars(env, url, u);
    }
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitles(
    JNIEnv *env, jobject thiz, jlong h) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    mpv_set_property_string(inst->mpv, "sid", "no");
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitlesAndSelect(
    JNIEnv *env, jobject thiz, jlong h, jint id) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    if (id <= 0) { mpv_set_property_string(inst->mpv, "sid", "no"); return; }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", id);
    mpv_set_property_string(inst->mpv, "sid", buf);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSubtitleDelayMs(
    JNIEnv *env, jobject thiz, jlong h, jint delayMs) {
    (void)env; (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%f", (double)delayMs / 1000.0);
    mpv_set_property_string(inst->mpv, "sub-delay", buf);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applySubtitleStyle(
    JNIEnv *env, jobject thiz, jlong h,
    jstring textColor, jstring backgroundColor, jstring outlineColor,
    jfloat outlineSize, jboolean bold, jfloat fontSize, jint subPos) {
    (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv) return;
    mpv_set_property_string(inst->mpv, "sub-ass-override", "yes");
    const char *s;
    if (textColor) { s = (*env)->GetStringUTFChars(env, textColor, NULL); if (s) { mpv_set_property_string(inst->mpv, "sub-color", s); (*env)->ReleaseStringUTFChars(env, textColor, s); } }
    if (backgroundColor) { s = (*env)->GetStringUTFChars(env, backgroundColor, NULL); if (s) { mpv_set_property_string(inst->mpv, "sub-back-color", s); (*env)->ReleaseStringUTFChars(env, backgroundColor, s); } }
    if (outlineColor) { s = (*env)->GetStringUTFChars(env, outlineColor, NULL); if (s) { mpv_set_property_string(inst->mpv, "sub-border-color", s); (*env)->ReleaseStringUTFChars(env, outlineColor, s); } }
    char buf[32];
    snprintf(buf, sizeof(buf), "%f", (double)outlineSize); mpv_set_property_string(inst->mpv, "sub-border-size", buf);
    mpv_set_property_string(inst->mpv, "sub-bold", bold ? "yes" : "no");
    snprintf(buf, sizeof(buf), "%f", (double)fontSize); mpv_set_property_string(inst->mpv, "sub-font-size", buf);
    snprintf(buf, sizeof(buf), "%d", subPos); mpv_set_property_string(inst->mpv, "sub-pos", buf);
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setProperty(
    JNIEnv *env, jobject thiz, jlong h, jstring name, jstring value) {
    (void)thiz;
    PlayerInstance *inst = h ? (PlayerInstance *)(intptr_t)h : NULL;
    if (!inst || !inst->mpv || !name || !value) return;
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    const char *v = (*env)->GetStringUTFChars(env, value, NULL);
    if (n && v) mpv_set_property_string(inst->mpv, n, v);
    if (n) (*env)->ReleaseStringUTFChars(env, name, n);
    if (v) (*env)->ReleaseStringUTFChars(env, value, v);
}

/* ------------------------------------------------------------------ */
/*  Stubs for methods not needed on Linux                              */
/* ------------------------------------------------------------------ */
JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applyWindowChrome(
    JNIEnv *env, jobject thiz, jlong hwnd, jboolean dm, jint cc, jint bc, jint tc) {
    (void)env; (void)thiz; (void)hwnd; (void)dm; (void)cc; (void)bc; (void)tc;
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setWindowBorderlessFullscreen(
    JNIEnv *env, jobject thiz, jlong hwnd, jboolean fs, jint x, jint y, jint w, jint ht) {
    (void)env; (void)thiz; (void)hwnd; (void)fs; (void)x; (void)y; (void)w; (void)ht;
}

JNIEXPORT jboolean JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_warmupWebView2(
    JNIEnv *env, jobject thiz, jstring url) {
    (void)env; (void)thiz; (void)url; return JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_shutdownWebView2Warmup(
    JNIEnv *env, jobject thiz) { (void)env; (void)thiz; }

JNIEXPORT jboolean JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isWaylandSession(
    JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return isWaylandSession() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_getX11WindowId(
    JNIEnv *env, jobject thiz, jlong awtPeerPtr) {
    (void)env; (void)thiz; (void)awtPeerPtr; return 0;
}

/* ------------------------------------------------------------------ */
/*  renderFrame — offscreen rendering via libmpv render API            */
/* ------------------------------------------------------------------ */
JNIEXPORT jboolean JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_renderFrame(
    JNIEnv *env, jobject thiz, jlong handle,
    jintArray dstPixels, jint dstW, jint dstH) {
    (void)thiz;
    if (!handle || !dstPixels || dstW <= 0 || dstH <= 0) return JNI_FALSE;

    PlayerInstance *inst = (PlayerInstance *)(intptr_t)handle;
    if (!inst->offscreenMode || !inst->renderCtx) return JNI_FALSE;

    eglMakeCurrent(inst->eglDisplay, inst->eglSurface, inst->eglSurface, inst->eglContext);

    fboResize(inst, dstW, dstH);

    mpv_opengl_fbo fbo = {(int)inst->offscreenFbo, dstW, dstH, GL_RGBA8};
    int flipY = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    if (mpv_render_context_render(inst->renderCtx, params) < 0) return JNI_FALSE;

    glBindFramebuffer(GL_FRAMEBUFFER, inst->offscreenFbo);
    glReadPixels(0, 0, dstW, dstH, GL_RGBA, GL_UNSIGNED_BYTE, inst->offscreenPixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    int size = dstW * dstH;
    jint *javaPixels = (*env)->GetIntArrayElements(env, dstPixels, NULL);
    if (javaPixels) {
        for (int i = 0; i < size; i++) {
            unsigned char r = inst->offscreenPixels[i * 4];
            unsigned char g = inst->offscreenPixels[i * 4 + 1];
            unsigned char b = inst->offscreenPixels[i * 4 + 2];
            unsigned char a = inst->offscreenPixels[i * 4 + 3];
            javaPixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        (*env)->ReleaseIntArrayElements(env, dstPixels, javaPixels, 0);
    }

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_renderFrameBytes(
    JNIEnv *env, jobject thiz, jlong handle,
    jbyteArray dstBytes, jint dstW, jint dstH) {
    (void)env; (void)thiz; (void)handle; (void)dstBytes; (void)dstW; (void)dstH;
    return JNI_FALSE;
}

/* ------------------------------------------------------------------ */
/*  JNI_OnLoad                                                         */
/* ------------------------------------------------------------------ */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *jvm, void *reserved) {
    (void)reserved;
    setlocale(LC_NUMERIC, "C");
#ifdef GDK_WINDOWING_X11
    XInitThreads();
#endif
    return JNI_VERSION_1_6;
}
