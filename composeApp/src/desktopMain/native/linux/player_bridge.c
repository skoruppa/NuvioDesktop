/*
 * Linux Native Player Bridge
 * 
 * Single C file compiled into libplayer_bridge.so.
 * Implements the JNI interface defined by NativePlayerBridge.kt.
 *
 * Architecture:
 * - All system libraries loaded via dlopen (libmpv, libEGL, libX11, libgtk-3, libwebkit2gtk-4.1, libcairo)
 * - EGL context created via eglGetPlatformDisplay (X11 platform)
 * - mpv with vo=libmpv + render API rendering into EGL surface
 * - WebKitGTK in-process offscreen rendering via webkit_web_view_get_snapshot:
 *   snapshot pixels (ARGB32/BGRA on LE) are uploaded as a GL texture and
 *   alpha-composited over the mpv frame before eglSwapBuffers.
 * - GTK event pump (g_main_context_iteration) runs in the render loop for WebKit processing.
 * - JNI callback for events via NativePlayerEventSink.onPlayerEvent(String, Double)
 */

#define _GNU_SOURCE  /* for strdup */

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <locale.h>
#include <time.h>

/* ============================================================================
 * EGL Type Definitions and Constants (loaded via dlopen)
 * ============================================================================ */

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef void* EGLNativeDisplayType;
typedef unsigned long EGLNativeWindowType;

typedef EGLDisplay (*fn_eglGetDisplay)(EGLNativeDisplayType);
typedef EGLBoolean (*fn_eglInitialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean (*fn_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLSurface (*fn_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);
typedef EGLContext (*fn_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean (*fn_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*fn_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef void* (*fn_eglGetProcAddress)(const char*);
typedef EGLBoolean (*fn_eglTerminate)(EGLDisplay);
typedef EGLBoolean (*fn_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*fn_eglDestroyContext)(EGLDisplay, EGLContext);
typedef EGLBoolean (*fn_eglSwapInterval)(EGLDisplay, EGLint);
typedef EGLBoolean (*fn_eglBindAPI)(unsigned int);

#define EGL_OPENGL_API            0x30A2
#define EGL_OPENGL_BIT            0x0008
#define EGL_NONE                  0x3038
#define EGL_RED_SIZE              0x3024
#define EGL_GREEN_SIZE            0x3025
#define EGL_BLUE_SIZE             0x3026
#define EGL_ALPHA_SIZE            0x3027
#define EGL_DEPTH_SIZE            0x3028
#define EGL_RENDERABLE_TYPE       0x3040
#define EGL_SURFACE_TYPE          0x3033
#define EGL_WINDOW_BIT            0x0004
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK       0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT   0x00000001
#define EGL_NO_CONTEXT            ((EGLContext)0)
#define EGL_NO_DISPLAY            ((EGLDisplay)0)
#define EGL_NO_SURFACE            ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY       ((EGLNativeDisplayType)0)

/* Global EGL function pointers (loaded once in gtk_thread_func) */
static fn_eglGetDisplay egl_getDisplay = NULL;
static fn_eglInitialize egl_initialize = NULL;
static fn_eglChooseConfig egl_chooseConfig = NULL;
static fn_eglCreateWindowSurface egl_createWindowSurface = NULL;
static fn_eglCreateContext egl_createContext = NULL;
static fn_eglMakeCurrent egl_makeCurrent = NULL;
static fn_eglSwapBuffers egl_swapBuffers = NULL;
static fn_eglGetProcAddress egl_getProcAddress = NULL;
static fn_eglTerminate egl_terminate = NULL;
static fn_eglDestroySurface egl_destroySurface = NULL;
static fn_eglDestroyContext egl_destroyContext = NULL;
static fn_eglSwapInterval egl_swapInterval = NULL;
static fn_eglBindAPI egl_bindAPI = NULL;

/* ============================================================================
 * OpenGL Function Pointers for Overlay Compositing
 * ============================================================================ */

/* GL constants */
#define GL_TEXTURE_2D            0x0DE1
#define GL_RGBA8                 0x8058
#define GL_BGRA                  0x80E1
#define GL_UNSIGNED_BYTE         0x1401
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_LINEAR                0x2601
#define GL_BLEND                 0x0BE2
#define GL_SRC_ALPHA             0x0302
#define GL_ONE_MINUS_SRC_ALPHA   0x0303
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_VERTEX_SHADER         0x8B31
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82
#define GL_ARRAY_BUFFER          0x8892
#define GL_STATIC_DRAW           0x88E4
#define GL_TRIANGLE_STRIP        0x0005
#define GL_FLOAT                 0x1406
#define GL_FALSE                 0
#define GL_TRUE                  1
#define GL_TEXTURE0              0x84C0
#define GL_INFO_LOG_LENGTH       0x8B84

typedef void (*fn_glGenTextures)(int, unsigned int*);
typedef void (*fn_glBindTexture)(unsigned int, unsigned int);
typedef void (*fn_glTexImage2D)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
typedef void (*fn_glTexSubImage2D)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
typedef void (*fn_glTexParameteri)(unsigned int, unsigned int, int);
typedef void (*fn_glEnable)(unsigned int);
typedef void (*fn_glDisable)(unsigned int);
typedef void (*fn_glBlendFunc)(unsigned int, unsigned int);
typedef unsigned int (*fn_glCreateShader)(unsigned int);
typedef void (*fn_glShaderSource)(unsigned int, int, const char**, const int*);
typedef void (*fn_glCompileShader)(unsigned int);
typedef void (*fn_glGetShaderiv)(unsigned int, unsigned int, int*);
typedef void (*fn_glGetShaderInfoLog)(unsigned int, int, int*, char*);
typedef unsigned int (*fn_glCreateProgram)(void);
typedef void (*fn_glAttachShader)(unsigned int, unsigned int);
typedef void (*fn_glLinkProgram)(unsigned int);
typedef void (*fn_glGetProgramiv)(unsigned int, unsigned int, int*);
typedef void (*fn_glGetProgramInfoLog)(unsigned int, int, int*, char*);
typedef void (*fn_glUseProgram)(unsigned int);
typedef void (*fn_glDeleteShader)(unsigned int);
typedef void (*fn_glGenVertexArrays)(int, unsigned int*);
typedef void (*fn_glBindVertexArray)(unsigned int);
typedef void (*fn_glGenBuffers)(int, unsigned int*);
typedef void (*fn_glBindBuffer)(unsigned int, unsigned int);
typedef void (*fn_glBufferData)(unsigned int, long, const void*, unsigned int);
typedef void (*fn_glEnableVertexAttribArray)(unsigned int);
typedef void (*fn_glVertexAttribPointer)(unsigned int, int, unsigned int, unsigned char, int, const void*);
typedef void (*fn_glDrawArrays)(unsigned int, int, int);
typedef int (*fn_glGetUniformLocation)(unsigned int, const char*);
typedef void (*fn_glUniform1i)(int, int);
typedef void (*fn_glActiveTexture)(unsigned int);
typedef void (*fn_glViewport)(int, int, int, int);
typedef void (*fn_glDeleteTextures)(int, const unsigned int*);
typedef void (*fn_glDeleteProgram)(unsigned int);
typedef void (*fn_glDeleteVertexArrays)(int, const unsigned int*);
typedef void (*fn_glDeleteBuffers)(int, const unsigned int*);
typedef void (*fn_glPixelStorei)(unsigned int, int);

static fn_glGenTextures gl_GenTextures = NULL;
static fn_glBindTexture gl_BindTexture = NULL;
static fn_glTexImage2D gl_TexImage2D = NULL;
static fn_glTexSubImage2D gl_TexSubImage2D = NULL;
static fn_glTexParameteri gl_TexParameteri = NULL;
static fn_glEnable gl_Enable = NULL;
static fn_glDisable gl_Disable = NULL;
static fn_glBlendFunc gl_BlendFunc = NULL;
static fn_glCreateShader gl_CreateShader = NULL;
static fn_glShaderSource gl_ShaderSource = NULL;
static fn_glCompileShader gl_CompileShader = NULL;
static fn_glGetShaderiv gl_GetShaderiv = NULL;
static fn_glGetShaderInfoLog gl_GetShaderInfoLog = NULL;
static fn_glCreateProgram gl_CreateProgram = NULL;
static fn_glAttachShader gl_AttachShader = NULL;
static fn_glLinkProgram gl_LinkProgram = NULL;
static fn_glGetProgramiv gl_GetProgramiv = NULL;
static fn_glGetProgramInfoLog gl_GetProgramInfoLog = NULL;
static fn_glUseProgram gl_UseProgram = NULL;
static fn_glDeleteShader gl_DeleteShader = NULL;
static fn_glGenVertexArrays gl_GenVertexArrays = NULL;
static fn_glBindVertexArray gl_BindVertexArray = NULL;
static fn_glGenBuffers gl_GenBuffers = NULL;
static fn_glBindBuffer gl_BindBuffer = NULL;
static fn_glBufferData gl_BufferData = NULL;
static fn_glEnableVertexAttribArray gl_EnableVertexAttribArray = NULL;
static fn_glVertexAttribPointer gl_VertexAttribPointer = NULL;
static fn_glDrawArrays gl_DrawArrays = NULL;
static fn_glGetUniformLocation gl_GetUniformLocation = NULL;
static fn_glUniform1i gl_Uniform1i = NULL;
static fn_glActiveTexture gl_ActiveTexture = NULL;
static fn_glViewport gl_Viewport = NULL;
static fn_glDeleteTextures gl_DeleteTextures = NULL;
static fn_glDeleteProgram gl_DeleteProgram = NULL;
static fn_glDeleteVertexArrays gl_DeleteVertexArrays = NULL;
static fn_glDeleteBuffers gl_DeleteBuffers = NULL;
static fn_glPixelStorei gl_PixelStorei = NULL;

static int gl_funcs_loaded = 0;

/* Primary offscreen window/webview (black-pass) — also persists to avoid WebKit crash */
static void *g_gtk_window_black = NULL;
static void *g_web_view_black = NULL;
static void *g_content_manager_black = NULL;
/* Current active PlayerInstance — used by script message/snapshot callbacks on reuse */
static void *g_current_player = NULL;
/* Snapshot pending flag — shared between render loop and async callback */
volatile int g_snapshot_pending = 0;
/* Dirty flag: set by mouse/input events, cleared after snapshot request.
 * When dirty, we request snapshot even if controls haven't visually changed,
 * ensuring hover states are captured with minimal latency. */
volatile int g_overlay_input_dirty = 0;
/* Draw-capture mode flag (reserved for future WPE integration) */
volatile int g_draw_capture_active = 0;

/* ============================================================================
 * MPV Constants and Enums (from mpv/client.h and mpv/render.h)
 * ============================================================================ */

enum mpv_format {
    MPV_FORMAT_NONE = 0,
    MPV_FORMAT_STRING = 1,
    MPV_FORMAT_OSD_STRING = 2,
    MPV_FORMAT_FLAG = 3,
    MPV_FORMAT_INT64 = 4,
    MPV_FORMAT_DOUBLE = 5,
    MPV_FORMAT_NODE = 6,
    MPV_FORMAT_NODE_ARRAY = 7,
    MPV_FORMAT_NODE_MAP = 8,
    MPV_FORMAT_BYTE_ARRAY = 9
};

enum mpv_event_id {
    MPV_EVENT_NONE = 0,
    MPV_EVENT_SHUTDOWN = 1,
    MPV_EVENT_LOG_MESSAGE = 2,
    MPV_EVENT_GET_PROPERTY_REPLY = 3,
    MPV_EVENT_SET_PROPERTY_REPLY = 4,
    MPV_EVENT_COMMAND_REPLY = 5,
    MPV_EVENT_START_FILE = 6,
    MPV_EVENT_END_FILE = 7,
    MPV_EVENT_FILE_LOADED = 8,
    MPV_EVENT_IDLE = 11,
    MPV_EVENT_TICK = 14,
    MPV_EVENT_CLIENT_MESSAGE = 16,
    MPV_EVENT_VIDEO_RECONFIG = 17,
    MPV_EVENT_AUDIO_RECONFIG = 18,
    MPV_EVENT_SEEK = 20,
    MPV_EVENT_PLAYBACK_RESTART = 21,
    MPV_EVENT_PROPERTY_CHANGE = 22,
    MPV_EVENT_QUEUE_OVERFLOW = 24
};

enum mpv_render_param_type {
    MPV_RENDER_PARAM_INVALID = 0,
    MPV_RENDER_PARAM_API_TYPE = 1,
    MPV_RENDER_PARAM_OPENGL_INIT_PARAMS = 2,
    MPV_RENDER_PARAM_OPENGL_FBO = 3,
    MPV_RENDER_PARAM_FLIP_Y = 4,
    MPV_RENDER_PARAM_DEPTH = 5,
    MPV_RENDER_PARAM_ICC_PROFILE = 6,
    MPV_RENDER_PARAM_AMBIENT_LIGHT = 7,
    MPV_RENDER_PARAM_ADVANCED_CONTROL = 10,
    MPV_RENDER_PARAM_NEXT_FRAME_INFO = 11,
    MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME = 12,
    MPV_RENDER_PARAM_SKIP_RENDERING = 13
};

#define MPV_RENDER_API_TYPE_OPENGL "opengl"

typedef struct mpv_node {
    union {
        char *string;
        int flag;
        int64_t int64;
        double double_;
        struct mpv_node_list *list;
    } u;
    enum mpv_format format;
} mpv_node;

typedef struct mpv_node_list {
    int num;
    mpv_node *values;
    char **keys;
} mpv_node_list;

typedef struct mpv_event {
    enum mpv_event_id event_id;
    int error;
    uint64_t reply_userdata;
    void *data;
} mpv_event;

typedef struct mpv_event_property {
    const char *name;
    enum mpv_format format;
    void *data;
} mpv_event_property;

typedef struct {
    void *(*get_proc_address)(void *ctx, const char *name);
    void *get_proc_address_ctx;
} mpv_opengl_init_params;

typedef struct {
    int fbo;
    int w;
    int h;
    int internal_format;
} mpv_opengl_fbo;

typedef struct {
    int type;
    void *data;
} mpv_render_param;

typedef void (*mpv_render_update_fn)(void *cb_ctx);

/* ============================================================================
 * GLib/GTK/GDK Type Definitions
 * ============================================================================ */

typedef int gboolean;
typedef unsigned int guint;
typedef void* gpointer;
typedef unsigned long gulong;
typedef void (*GCallback)(void);
typedef gboolean (*GSourceFunc)(gpointer data);
typedef void (*GDestroyNotify)(gpointer data);

typedef struct { double red; double green; double blue; double alpha; } GdkRGBA;

/* ============================================================================
 * Function Pointer Typedefs
 * ============================================================================ */

/* mpv functions */
typedef void* (*fn_mpv_create)(void);
typedef int (*fn_mpv_initialize)(void *ctx);
typedef void (*fn_mpv_destroy)(void *ctx);
typedef int (*fn_mpv_set_option_string)(void *ctx, const char *name, const char *data);
typedef int (*fn_mpv_set_property)(void *ctx, const char *name, enum mpv_format format, void *data);
typedef int (*fn_mpv_set_property_string)(void *ctx, const char *name, const char *data);
typedef int (*fn_mpv_get_property)(void *ctx, const char *name, enum mpv_format format, void *data);
typedef char* (*fn_mpv_get_property_string)(void *ctx, const char *name);
typedef int (*fn_mpv_command)(void *ctx, const char **args);
typedef int (*fn_mpv_command_string)(void *ctx, const char *args);
typedef int (*fn_mpv_command_async)(void *ctx, uint64_t reply_userdata, const char **args);
typedef int (*fn_mpv_observe_property)(void *ctx, uint64_t reply_userdata, const char *name, enum mpv_format format);
typedef mpv_event* (*fn_mpv_wait_event)(void *ctx, double timeout);
typedef void (*fn_mpv_free)(void *data);
typedef void (*fn_mpv_free_node_contents)(mpv_node *node);
typedef int (*fn_mpv_render_context_create)(void **res, void *mpv, mpv_render_param *params);
typedef void (*fn_mpv_render_context_set_update_callback)(void *ctx, mpv_render_update_fn callback, void *callback_ctx);
typedef int (*fn_mpv_render_context_render)(void *ctx, mpv_render_param *params);
typedef void (*fn_mpv_render_context_free)(void *ctx);
typedef void (*fn_mpv_render_context_report_swap)(void *ctx);
typedef uint64_t (*fn_mpv_render_context_update)(void *ctx);
typedef const char* (*fn_mpv_error_string)(int error);
typedef int (*fn_mpv_set_option)(void *ctx, const char *name, enum mpv_format format, void *data);

/* GTK functions */
typedef gboolean (*fn_gtk_init_check)(int *argc, char ***argv);
typedef gboolean (*fn_g_main_context_iteration)(void *context, gboolean may_block);
typedef guint (*fn_g_timeout_add)(guint interval, GSourceFunc function, gpointer data);
typedef guint (*fn_g_idle_add)(GSourceFunc function, gpointer data);
typedef gboolean (*fn_g_source_remove)(guint tag);
typedef void* (*fn_gtk_window_new)(int type);
typedef void (*fn_gtk_window_set_default_size)(void *window, int width, int height);
typedef void (*fn_gtk_widget_realize)(void *widget);
typedef void (*fn_gtk_widget_show_all)(void *widget);
typedef void (*fn_gtk_widget_show)(void *widget);
typedef void (*fn_gtk_widget_destroy)(void *widget);
typedef void (*fn_gtk_widget_set_visual)(void *widget, void *visual);
typedef void (*fn_gtk_widget_get_screen)(void *widget);
typedef void* (*fn_gtk_widget_get_window)(void *widget);
typedef void (*fn_gtk_widget_set_size_request)(void *widget, int width, int height);
typedef void (*fn_gtk_widget_set_app_paintable)(void *widget, gboolean app_paintable);
typedef void* (*fn_gtk_fixed_new)(void);
typedef void (*fn_gtk_fixed_put)(void *fixed, void *widget, int x, int y);
typedef void (*fn_gtk_container_add)(void *container, void *widget);
typedef gulong (*fn_g_signal_connect_data)(void *instance, const char *signal_name, GCallback handler, gpointer data, void *destroy_data, int flags);
typedef void* (*fn_gdk_screen_get_rgba_visual)(void *screen);
typedef void* (*fn_gdk_screen_get_default)(void);
typedef void* (*fn_gtk_widget_get_screen_fn)(void *widget);
typedef void* (*fn_gdk_x11_window_get_xid)(void *gdk_window);
typedef void* (*fn_gdk_x11_display_get_xdisplay)(void *display);
typedef void* (*fn_gdk_display_get_default)(void);
typedef void (*fn_gtk_window_resize)(void *window, int width, int height);
typedef void (*fn_gtk_window_move)(void *window, int x, int y);
typedef void (*fn_gtk_widget_set_opacity)(void *widget, double opacity);
typedef void* (*fn_gtk_plug_new)(unsigned long socket_id);
typedef unsigned long (*fn_gtk_plug_get_id)(void *plug);

/* WebKit functions */
typedef void* (*fn_webkit_web_view_new_with_user_content_manager)(void *mgr);
typedef void* (*fn_webkit_user_content_manager_new)(void);
typedef gboolean (*fn_webkit_user_content_manager_register_script_message_handler)(void *mgr, const char *name);
typedef void (*fn_webkit_web_view_load_uri)(void *web_view, const char *uri);
typedef void (*fn_webkit_web_view_run_javascript)(void *web_view, const char *script, void *cancellable, void *callback, void *user_data);
typedef void (*fn_webkit_web_view_set_background_color)(void *web_view, const GdkRGBA *rgba);
typedef void* (*fn_webkit_web_view_get_settings)(void *web_view);
typedef void (*fn_webkit_settings_set_enable_developer_extras)(void *settings, gboolean enabled);
typedef void* (*fn_webkit_javascript_result_get_js_value)(void *js_result);

/* JavaScriptCore functions */
typedef char* (*fn_jsc_value_to_json)(void *value, unsigned int indent);
typedef char* (*fn_jsc_value_to_string)(void *value);
typedef gboolean (*fn_jsc_value_is_string)(void *value);

/* WebKitGTK offscreen + cairo functions (for in-process overlay rendering) */
typedef void* (*fn_gtk_offscreen_window_new)(void);
typedef void* (*fn_gtk_offscreen_window_get_surface)(void *window);
typedef void* (*fn_webkit_settings_new)(void);
typedef void (*fn_webkit_settings_set_hardware_acceleration_policy)(void *settings, int policy);
typedef unsigned char* (*fn_cairo_image_surface_get_data)(void *surface);
typedef int (*fn_cairo_image_surface_get_width)(void *surface);
typedef int (*fn_cairo_image_surface_get_height)(void *surface);
typedef int (*fn_cairo_image_surface_get_stride)(void *surface);
typedef void (*fn_cairo_surface_flush)(void *surface);

/* GLib memory */
typedef void (*fn_g_free)(void *mem);

/* ============================================================================
 * Library Context (global dlopen handles + function pointers)
 * ============================================================================ */

typedef struct {
    void *mpv_lib;
    void *gtk_lib;
    void *webkit_lib;
    void *jsc_lib;

    /* mpv */
    fn_mpv_create mpv_create;
    fn_mpv_initialize mpv_initialize;
    fn_mpv_destroy mpv_destroy;
    fn_mpv_set_option_string mpv_set_option_string;
    fn_mpv_set_property mpv_set_property;
    fn_mpv_set_property_string mpv_set_property_string;
    fn_mpv_get_property mpv_get_property;
    fn_mpv_get_property_string mpv_get_property_string;
    fn_mpv_command mpv_command;
    fn_mpv_command_string mpv_command_string;
    fn_mpv_command_async mpv_command_async;
    fn_mpv_observe_property mpv_observe_property;
    fn_mpv_wait_event mpv_wait_event;
    fn_mpv_free mpv_free;
    fn_mpv_free_node_contents mpv_free_node_contents;
    fn_mpv_render_context_create mpv_render_context_create;
    fn_mpv_render_context_set_update_callback mpv_render_context_set_update_callback;
    fn_mpv_render_context_render mpv_render_context_render;
    fn_mpv_render_context_free mpv_render_context_free;
    fn_mpv_render_context_report_swap mpv_render_context_report_swap;
    fn_mpv_render_context_update mpv_render_context_update;
    fn_mpv_error_string mpv_error_string;
    fn_mpv_set_option mpv_set_option;

    /* GTK / GLib */
    fn_gtk_init_check gtk_init_check;
    fn_g_main_context_iteration g_main_context_iteration;
    fn_g_timeout_add g_timeout_add;
    fn_g_idle_add g_idle_add;
    fn_g_source_remove g_source_remove;
    fn_gtk_window_new gtk_window_new;
    fn_gtk_window_set_default_size gtk_window_set_default_size;
    fn_gtk_widget_realize gtk_widget_realize;
    fn_gtk_widget_show_all gtk_widget_show_all;
    fn_gtk_widget_show gtk_widget_show;
    fn_gtk_widget_destroy gtk_widget_destroy;
    fn_gtk_widget_set_visual gtk_widget_set_visual;
    fn_gtk_widget_get_window gtk_widget_get_window;
    fn_gtk_widget_set_size_request gtk_widget_set_size_request;
    fn_gtk_widget_set_app_paintable gtk_widget_set_app_paintable;
    fn_gtk_fixed_new gtk_fixed_new;
    fn_gtk_fixed_put gtk_fixed_put;
    fn_gtk_container_add gtk_container_add;
    fn_g_signal_connect_data g_signal_connect_data;
    fn_gdk_screen_get_rgba_visual gdk_screen_get_rgba_visual;
    fn_gdk_screen_get_default gdk_screen_get_default;
    fn_gtk_widget_get_screen_fn gtk_widget_get_screen;
    fn_gdk_x11_window_get_xid gdk_x11_window_get_xid;
    fn_gdk_x11_display_get_xdisplay gdk_x11_display_get_xdisplay;
    fn_gdk_display_get_default gdk_display_get_default;
    fn_gtk_window_resize gtk_window_resize;
    fn_gtk_window_move gtk_window_move;
    fn_gtk_widget_set_opacity gtk_widget_set_opacity;
    fn_gtk_plug_new gtk_plug_new;
    fn_gtk_plug_get_id gtk_plug_get_id;
    fn_g_free g_free;

    /* WebKit */
    fn_webkit_web_view_new_with_user_content_manager webkit_web_view_new_with_user_content_manager;
    fn_webkit_user_content_manager_new webkit_user_content_manager_new;
    fn_webkit_user_content_manager_register_script_message_handler webkit_user_content_manager_register_script_message_handler;
    fn_webkit_web_view_load_uri webkit_web_view_load_uri;
    fn_webkit_web_view_run_javascript webkit_web_view_run_javascript;
    fn_webkit_web_view_set_background_color webkit_web_view_set_background_color;
    fn_webkit_web_view_get_settings webkit_web_view_get_settings;
    fn_webkit_settings_set_enable_developer_extras webkit_settings_set_enable_developer_extras;
    fn_webkit_javascript_result_get_js_value webkit_javascript_result_get_js_value;

    /* JSC */
    fn_jsc_value_to_json jsc_value_to_json;
    fn_jsc_value_to_string jsc_value_to_string;
    fn_jsc_value_is_string jsc_value_is_string;

    /* WebKitGTK offscreen + cairo (in-process overlay) */
    void *cairo_lib;
    fn_gtk_offscreen_window_new gtk_offscreen_window_new;
    fn_gtk_offscreen_window_get_surface gtk_offscreen_window_get_surface;
    fn_webkit_settings_new webkit_settings_new;
    fn_webkit_settings_set_hardware_acceleration_policy webkit_settings_set_hardware_acceleration_policy;
    fn_cairo_image_surface_get_data cairo_image_surface_get_data;
    fn_cairo_image_surface_get_width cairo_image_surface_get_width;
    fn_cairo_image_surface_get_height cairo_image_surface_get_height;
    fn_cairo_image_surface_get_stride cairo_image_surface_get_stride;
    fn_cairo_surface_flush cairo_surface_flush;
} LibraryContext;

static LibraryContext g_libs = {0};
static int g_libs_loaded = 0;
static pthread_mutex_t g_libs_mutex = PTHREAD_MUTEX_INITIALIZER;


/* ============================================================================
 * PlayerInstance Structure
 * ============================================================================ */

typedef struct {
    void *mpv;                      /* mpv_handle* */
    void *render_ctx;               /* mpv_render_context* */
    void *gtk_window;               /* GtkPlug* or GtkWindow* */
    void *web_view;                 /* WebKitWebView* */
    void *content_manager;          /* WebKitUserContentManager* */
    void *overlay_fixed;            /* GtkFixed* container */
    pthread_t gtk_thread;
    volatile int running;           /* loop control */
    int controls_ready;             /* 1 after "controlsReady" */
    volatile int controls_visible;  /* 1 when UI overlay is shown, 0 when hidden */
    char *pending_controls_json;    /* stored until controlsReady */
    JavaVM *jvm;
    jobject event_sink;             /* global ref */
    jmethodID event_method;         /* onPlayerEvent */
    unsigned long host_window;      /* X11 Window ID */
    unsigned long render_window;    /* 32-bit depth child window for EGL + overlay */
    guint update_timer_id;          /* g_timeout source */
    volatile int is_loading;
    volatile int is_ended;
    /* Track external subtitle IDs for clearExternalSubtitles */
    int *external_sub_ids;
    int external_sub_count;
    int external_sub_capacity;
    /* Synchronization for create */
    pthread_mutex_t init_mutex;
    pthread_cond_t init_cond;
    int init_done;
    /* Source parameters (needed for GTK thread init) */
    char *source_url;
    char **header_lines;
    int header_count;
    int play_when_ready;
    long long initial_position_ms;
    char *controls_page_url;
    int decoder_priority;
    /* Overlay — pixel staging for GL texture upload */
    unsigned char *overlay_staging;      /* Staging buffer (BGRA pixels from snapshot/external) */
    int overlay_staging_width;
    int overlay_staging_height;
    pthread_mutex_t overlay_mutex;       /* Protects staging buffer */
    volatile int overlay_dirty;          /* 1 when new pixels copied to staging */
    unsigned int overlay_texture;        /* GL texture ID */
    unsigned int overlay_shader;         /* Shader program for alpha-blended quad */
    unsigned int overlay_vao;            /* VAO for fullscreen quad */
    unsigned int overlay_vbo;            /* VBO for quad vertices */
    int overlay_initialized;             /* 1 after GL resources created */
    /* EGL state for libmpv render API */
    void *egl_display;
    void *egl_surface;
    void *egl_context;
    void *egl_lib;
    /* Render signal */
    volatile int render_needed;
    pthread_mutex_t render_mutex;
    pthread_cond_t render_cond;
} PlayerInstance;

/* ============================================================================
 * Library Loading
 * ============================================================================ */

#define LOAD_SYM(lib, name) do { \
    g_libs.name = (fn_##name)dlsym(lib, #name); \
    if (!g_libs.name) { \
        fprintf(stderr, "[player_bridge] Failed to load symbol: %s: %s\n", #name, dlerror()); \
        return 0; \
    } \
} while(0)

#define LOAD_SYM_OPT(lib, name) do { \
    g_libs.name = (fn_##name)dlsym(lib, #name); \
} while(0)

static int load_libraries(void) {
    pthread_mutex_lock(&g_libs_mutex);
    if (g_libs_loaded) {
        pthread_mutex_unlock(&g_libs_mutex);
        return 1;
    }

    /* Load libmpv ONLY — GTK/WebKit loaded later on render thread to avoid
     * premature GDK type registration (conflict with -Djdk.gtk.version=0) */
    g_libs.mpv_lib = dlopen("libmpv.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libs.mpv_lib) {
        g_libs.mpv_lib = dlopen("libmpv.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_libs.mpv_lib) {
        fprintf(stderr, "[player_bridge] Failed to load libmpv: %s\n", dlerror());
        pthread_mutex_unlock(&g_libs_mutex);
        return 0;
    }

    /* Resolve mpv symbols */
    LOAD_SYM(g_libs.mpv_lib, mpv_create);
    LOAD_SYM(g_libs.mpv_lib, mpv_initialize);
    LOAD_SYM(g_libs.mpv_lib, mpv_destroy);
    LOAD_SYM(g_libs.mpv_lib, mpv_set_option_string);
    LOAD_SYM(g_libs.mpv_lib, mpv_set_property);
    LOAD_SYM(g_libs.mpv_lib, mpv_set_property_string);
    LOAD_SYM(g_libs.mpv_lib, mpv_get_property);
    LOAD_SYM(g_libs.mpv_lib, mpv_get_property_string);
    LOAD_SYM(g_libs.mpv_lib, mpv_command);
    LOAD_SYM(g_libs.mpv_lib, mpv_command_string);
    LOAD_SYM(g_libs.mpv_lib, mpv_command_async);
    LOAD_SYM(g_libs.mpv_lib, mpv_observe_property);
    LOAD_SYM(g_libs.mpv_lib, mpv_wait_event);
    LOAD_SYM(g_libs.mpv_lib, mpv_free);
    LOAD_SYM(g_libs.mpv_lib, mpv_free_node_contents);
    LOAD_SYM(g_libs.mpv_lib, mpv_render_context_create);
    LOAD_SYM(g_libs.mpv_lib, mpv_render_context_set_update_callback);
    LOAD_SYM(g_libs.mpv_lib, mpv_render_context_render);
    LOAD_SYM(g_libs.mpv_lib, mpv_render_context_free);
    LOAD_SYM(g_libs.mpv_lib, mpv_render_context_report_swap);
    LOAD_SYM(g_libs.mpv_lib, mpv_render_context_update);
    LOAD_SYM(g_libs.mpv_lib, mpv_error_string);
    LOAD_SYM(g_libs.mpv_lib, mpv_set_option);

    g_libs_loaded = 1;
    pthread_mutex_unlock(&g_libs_mutex);
    return 1;
}

/* Load GTK/WebKit/Cairo — called on render thread AFTER gtk_init_check */
static int g_gtk_libs_loaded = 0;
static int load_gtk_libraries(void) {
    if (g_gtk_libs_loaded) return 1;

    /* Load libgtk-3 */
    g_libs.gtk_lib = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libs.gtk_lib) {
        fprintf(stderr, "[player_bridge] Failed to load libgtk-3.so.0: %s\n", dlerror());
        return 0;
    }

    /* Load libwebkit2gtk-4.1 */
    g_libs.webkit_lib = dlopen("libwebkit2gtk-4.1.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libs.webkit_lib) {
        fprintf(stderr, "[player_bridge] Failed to load libwebkit2gtk-4.1.so.0: %s\n", dlerror());
        return 0;
    }

    /* Load libjavascriptcoregtk-4.1 */
    g_libs.jsc_lib = dlopen("libjavascriptcoregtk-4.1.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libs.jsc_lib) {
        fprintf(stderr, "[player_bridge] Failed to load libjavascriptcoregtk-4.1.so.0: %s\n", dlerror());
        return 0;
    }

    /* Resolve GTK/GLib symbols */
    LOAD_SYM(g_libs.gtk_lib, gtk_init_check);
    LOAD_SYM(g_libs.gtk_lib, g_main_context_iteration);
    LOAD_SYM(g_libs.gtk_lib, g_timeout_add);
    LOAD_SYM(g_libs.gtk_lib, g_idle_add);
    LOAD_SYM(g_libs.gtk_lib, g_source_remove);
    LOAD_SYM(g_libs.gtk_lib, gtk_window_new);
    LOAD_SYM(g_libs.gtk_lib, gtk_window_set_default_size);
    LOAD_SYM(g_libs.gtk_lib, gtk_widget_realize);
    LOAD_SYM(g_libs.gtk_lib, gtk_widget_show_all);
    LOAD_SYM(g_libs.gtk_lib, gtk_widget_show);
    LOAD_SYM(g_libs.gtk_lib, gtk_widget_destroy);
    LOAD_SYM(g_libs.gtk_lib, gtk_widget_set_visual);
    LOAD_SYM(g_libs.gtk_lib, gtk_widget_get_window);
    LOAD_SYM(g_libs.gtk_lib, gtk_widget_set_size_request);
    LOAD_SYM(g_libs.gtk_lib, gtk_widget_set_app_paintable);
    LOAD_SYM(g_libs.gtk_lib, gtk_fixed_new);
    LOAD_SYM(g_libs.gtk_lib, gtk_fixed_put);
    LOAD_SYM(g_libs.gtk_lib, gtk_container_add);
    LOAD_SYM(g_libs.gtk_lib, g_signal_connect_data);
    LOAD_SYM(g_libs.gtk_lib, gdk_screen_get_rgba_visual);
    LOAD_SYM(g_libs.gtk_lib, gdk_screen_get_default);
    LOAD_SYM(g_libs.gtk_lib, gdk_display_get_default);
    LOAD_SYM(g_libs.gtk_lib, gtk_window_resize);
    LOAD_SYM(g_libs.gtk_lib, gtk_window_move);
    LOAD_SYM(g_libs.gtk_lib, gtk_plug_new);
    LOAD_SYM(g_libs.gtk_lib, gtk_plug_get_id);

    /* gtk_widget_get_screen has a different name than what we typedef'd */
    g_libs.gtk_widget_get_screen = (fn_gtk_widget_get_screen_fn)dlsym(g_libs.gtk_lib, "gtk_widget_get_screen");
    if (!g_libs.gtk_widget_get_screen) {
        fprintf(stderr, "[player_bridge] Failed to load symbol: gtk_widget_get_screen: %s\n", dlerror());
        pthread_mutex_unlock(&g_libs_mutex);
        return 0;
    }

    /* GDK X11 symbols - may be in libgdk-3 which is loaded with libgtk-3 */
    g_libs.gdk_x11_window_get_xid = (fn_gdk_x11_window_get_xid)dlsym(g_libs.gtk_lib, "gdk_x11_window_get_xid");
    g_libs.gdk_x11_display_get_xdisplay = (fn_gdk_x11_display_get_xdisplay)dlsym(g_libs.gtk_lib, "gdk_x11_display_get_xdisplay");

    /* g_free from GLib (loaded with GTK) */
    g_libs.g_free = (fn_g_free)dlsym(g_libs.gtk_lib, "g_free");
    if (!g_libs.g_free) {
        /* Try libglib directly */
        void *glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (glib) {
            g_libs.g_free = (fn_g_free)dlsym(glib, "g_free");
        }
        if (!g_libs.g_free) {
            /* Fallback to free() */
            g_libs.g_free = (fn_g_free)free;
        }
    }

    /* Resolve WebKit symbols */
    LOAD_SYM(g_libs.webkit_lib, webkit_web_view_new_with_user_content_manager);
    LOAD_SYM(g_libs.webkit_lib, webkit_user_content_manager_new);
    LOAD_SYM(g_libs.webkit_lib, webkit_user_content_manager_register_script_message_handler);
    LOAD_SYM(g_libs.webkit_lib, webkit_web_view_load_uri);
    LOAD_SYM(g_libs.webkit_lib, webkit_web_view_run_javascript);
    LOAD_SYM(g_libs.webkit_lib, webkit_web_view_set_background_color);
    LOAD_SYM(g_libs.webkit_lib, webkit_web_view_get_settings);
    LOAD_SYM(g_libs.webkit_lib, webkit_settings_set_enable_developer_extras);
    LOAD_SYM(g_libs.webkit_lib, webkit_javascript_result_get_js_value);

    /* Resolve JSC symbols */
    LOAD_SYM(g_libs.jsc_lib, jsc_value_to_json);
    LOAD_SYM_OPT(g_libs.jsc_lib, jsc_value_to_string);
    LOAD_SYM_OPT(g_libs.jsc_lib, jsc_value_is_string);

    /* Resolve WebKitGTK offscreen symbols (for in-process overlay rendering) */
    LOAD_SYM(g_libs.gtk_lib, gtk_offscreen_window_new);
    LOAD_SYM(g_libs.gtk_lib, gtk_offscreen_window_get_surface);
    LOAD_SYM(g_libs.webkit_lib, webkit_settings_new);
    LOAD_SYM(g_libs.webkit_lib, webkit_settings_set_hardware_acceleration_policy);

    /* Load cairo symbols — try libcairo.so.2 first, fall back to RTLD_DEFAULT (GTK pulls it in) */
    g_libs.cairo_lib = dlopen("libcairo.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libs.cairo_lib) {
        g_libs.cairo_lib = dlopen("libcairo.so", RTLD_NOW | RTLD_GLOBAL);
    }
    {
        void *cairo_handle = g_libs.cairo_lib ? g_libs.cairo_lib : RTLD_DEFAULT;
        g_libs.cairo_image_surface_get_data = (fn_cairo_image_surface_get_data)dlsym(cairo_handle, "cairo_image_surface_get_data");
        g_libs.cairo_image_surface_get_width = (fn_cairo_image_surface_get_width)dlsym(cairo_handle, "cairo_image_surface_get_width");
        g_libs.cairo_image_surface_get_height = (fn_cairo_image_surface_get_height)dlsym(cairo_handle, "cairo_image_surface_get_height");
        g_libs.cairo_image_surface_get_stride = (fn_cairo_image_surface_get_stride)dlsym(cairo_handle, "cairo_image_surface_get_stride");
        g_libs.cairo_surface_flush = (fn_cairo_surface_flush)dlsym(cairo_handle, "cairo_surface_flush");
        if (!g_libs.cairo_image_surface_get_data || !g_libs.cairo_surface_flush) {
            fprintf(stderr, "[player_bridge] Warning: cairo symbols not found — overlay snapshot disabled\n");
        }
    }

    g_gtk_libs_loaded = 1;
    return 1;
}


/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void* get_gl_proc_address(void *ctx, const char *name) {
    (void)ctx;
    /* Try eglGetProcAddress first (for GL extension functions), then dlsym */
    if (egl_getProcAddress) {
        void *addr = egl_getProcAddress(name);
        if (addr) return addr;
    }
    return dlsym(RTLD_DEFAULT, name);
}

/* Load all GL function pointers needed for overlay compositing */
static int load_gl_functions(void) {
    if (gl_funcs_loaded) return 1;

    #define LOAD_GL(name) do { \
        gl_##name = (fn_gl##name)get_gl_proc_address(NULL, "gl" #name); \
        if (!gl_##name) { fprintf(stderr, "[player_bridge] Failed to load GL func: gl%s\n", #name); return 0; } \
    } while(0)

    LOAD_GL(GenTextures);
    LOAD_GL(BindTexture);
    LOAD_GL(TexImage2D);
    LOAD_GL(TexSubImage2D);
    LOAD_GL(TexParameteri);
    LOAD_GL(Enable);
    LOAD_GL(Disable);
    LOAD_GL(BlendFunc);
    LOAD_GL(CreateShader);
    LOAD_GL(ShaderSource);
    LOAD_GL(CompileShader);
    LOAD_GL(GetShaderiv);
    LOAD_GL(GetShaderInfoLog);
    LOAD_GL(CreateProgram);
    LOAD_GL(AttachShader);
    LOAD_GL(LinkProgram);
    LOAD_GL(GetProgramiv);
    LOAD_GL(GetProgramInfoLog);
    LOAD_GL(UseProgram);
    LOAD_GL(DeleteShader);
    LOAD_GL(GenVertexArrays);
    LOAD_GL(BindVertexArray);
    LOAD_GL(GenBuffers);
    LOAD_GL(BindBuffer);
    LOAD_GL(BufferData);
    LOAD_GL(EnableVertexAttribArray);
    LOAD_GL(VertexAttribPointer);
    LOAD_GL(DrawArrays);
    LOAD_GL(GetUniformLocation);
    LOAD_GL(Uniform1i);
    LOAD_GL(ActiveTexture);
    LOAD_GL(Viewport);
    LOAD_GL(DeleteTextures);
    LOAD_GL(DeleteProgram);
    LOAD_GL(DeleteVertexArrays);
    LOAD_GL(DeleteBuffers);
    LOAD_GL(PixelStorei);

    #undef LOAD_GL
    gl_funcs_loaded = 1;
    fprintf(stderr, "[player_bridge] GL overlay functions loaded\n");
    return 1;
}

/* Compile a GLSL shader, returns 0 on failure */
static unsigned int compile_shader(unsigned int type, const char *source) {
    unsigned int shader = gl_CreateShader(type);
    gl_ShaderSource(shader, 1, &source, NULL);
    gl_CompileShader(shader);
    int status = 0;
    gl_GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        gl_GetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[player_bridge] Shader compile error: %s\n", log);
        gl_DeleteShader(shader);
        return 0;
    }
    return shader;
}

/* Initialize overlay GL resources (texture, shader, VAO/VBO) */
static void init_overlay_gl(PlayerInstance *p) {
    if (p->overlay_initialized) return;
    if (!gl_funcs_loaded && !load_gl_functions()) return;

    /* Vertex shader */
    static const char *vs_source =
        "#version 330 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "layout(location=1) in vec2 aTexCoord;\n"
        "out vec2 vTexCoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "    vTexCoord = aTexCoord;\n"
        "}\n";

    /* Fragment shader — premultiplied alpha passthrough.
     * With RGBA visual on GtkOffscreenWindow, cairo surface has true alpha channel.
     * Cairo ARGB32 is premultiplied, so we un-premultiply and use straight alpha blending. */
    static const char *fs_source =
        "#version 330 core\n"
        "in vec2 vTexCoord;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D uTexture;\n"
        "void main() {\n"
        "    vec4 c = texture(uTexture, vTexCoord);\n"
        "    if (c.a < 0.004) {\n"
        "        FragColor = vec4(0.0);\n"
        "    } else {\n"
        "        FragColor = vec4(c.rgb / c.a, c.a);\n"
        "    }\n"
        "}\n";

    unsigned int vs = compile_shader(GL_VERTEX_SHADER, vs_source);
    unsigned int fs = compile_shader(GL_FRAGMENT_SHADER, fs_source);
    if (!vs || !fs) {
        if (vs) gl_DeleteShader(vs);
        if (fs) gl_DeleteShader(fs);
        fprintf(stderr, "[player_bridge] Failed to compile overlay shaders\n");
        return;
    }

    p->overlay_shader = gl_CreateProgram();
    gl_AttachShader(p->overlay_shader, vs);
    gl_AttachShader(p->overlay_shader, fs);
    gl_LinkProgram(p->overlay_shader);
    gl_DeleteShader(vs);
    gl_DeleteShader(fs);

    int link_status = 0;
    gl_GetProgramiv(p->overlay_shader, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        char log[512];
        gl_GetProgramInfoLog(p->overlay_shader, sizeof(log), NULL, log);
        fprintf(stderr, "[player_bridge] Shader link error: %s\n", log);
        gl_DeleteProgram(p->overlay_shader);
        p->overlay_shader = 0;
        return;
    }

    /* Fullscreen quad: position (x,y) + texcoord (u,v) — triangle strip
     * Note: Y-flipped texcoords because WebKit snapshot paints top-down */
    static const float quad_vertices[] = {
        /* pos       texcoord */
        -1.0f, -1.0f,  0.0f, 1.0f,  /* bottom-left  */
         1.0f, -1.0f,  1.0f, 1.0f,  /* bottom-right */
        -1.0f,  1.0f,  0.0f, 0.0f,  /* top-left     */
         1.0f,  1.0f,  1.0f, 0.0f,  /* top-right    */
    };

    gl_GenVertexArrays(1, &p->overlay_vao);
    gl_GenBuffers(1, &p->overlay_vbo);
    gl_BindVertexArray(p->overlay_vao);
    gl_BindBuffer(GL_ARRAY_BUFFER, p->overlay_vbo);
    gl_BufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    /* aPos = location 0 */
    gl_EnableVertexAttribArray(0);
    gl_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    /* aTexCoord = location 1 */
    gl_EnableVertexAttribArray(1);
    gl_VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    gl_BindVertexArray(0);

    p->overlay_initialized = 1;
    fprintf(stderr, "[player_bridge] Overlay GL resources initialized (shader=%u vao=%u)\n",
            p->overlay_shader, p->overlay_vao);
}

/* Render the overlay texture as a fullscreen alpha-blended quad */
static void render_overlay(PlayerInstance *p, int viewport_w, int viewport_h) {
    if (!p->overlay_initialized || !p->overlay_texture) return;

    gl_Viewport(0, 0, viewport_w, viewport_h);
    gl_Enable(GL_BLEND);
    gl_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl_UseProgram(p->overlay_shader);
    /* Bind black-pass to texture unit 0 */
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, p->overlay_texture);
    gl_Uniform1i(gl_GetUniformLocation(p->overlay_shader, "uTexture"), 0);
    gl_BindVertexArray(p->overlay_vao);
    gl_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_BindVertexArray(0);
    gl_UseProgram(0);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_Disable(GL_BLEND);
}

/* Cleanup overlay GL resources */
static void cleanup_overlay_gl(PlayerInstance *p) {
    if (p->overlay_texture) {
        gl_DeleteTextures(1, &p->overlay_texture);
        p->overlay_texture = 0;
    }
    if (p->overlay_shader) {
        gl_DeleteProgram(p->overlay_shader);
        p->overlay_shader = 0;
    }
    if (p->overlay_vao) {
        gl_DeleteVertexArrays(1, &p->overlay_vao);
        p->overlay_vao = 0;
    }
    if (p->overlay_vbo) {
        gl_DeleteBuffers(1, &p->overlay_vbo);
        p->overlay_vbo = 0;
    }
    p->overlay_initialized = 0;
}

static void send_player_event(PlayerInstance *p, const char *type, double value) {
    if (!p || !p->jvm || !p->event_sink) return;

    JNIEnv *env = NULL;
    int attached = 0;
    jint rc = (*p->jvm)->GetEnv(p->jvm, (void**)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*p->jvm)->AttachCurrentThread(p->jvm, (void**)&env, NULL) == 0) {
            attached = 1;
        } else {
            return;
        }
    } else if (rc != JNI_OK) {
        return;
    }

    jstring jtype = (*env)->NewStringUTF(env, type);
    if (jtype) {
        (*env)->CallVoidMethod(env, p->event_sink, p->event_method, jtype, (jdouble)value);
        (*env)->DeleteLocalRef(env, jtype);
    }

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }

    if (attached) {
        (*p->jvm)->DetachCurrentThread(p->jvm);
    }
}

/* Minimal JSON parser for {"type":"...","value":...} */
static int parse_message_json(const char *json, char *type_out, int type_max, double *value_out) {
    if (!json || !type_out || !value_out) return 0;
    type_out[0] = '\0';
    *value_out = 0.0;

    /* Find "type" field */
    const char *tp = strstr(json, "\"type\"");
    if (!tp) return 0;
    tp = strchr(tp + 6, ':');
    if (!tp) return 0;
    tp++;
    while (*tp == ' ' || *tp == '\t') tp++;
    if (*tp != '"') return 0;
    tp++;
    int i = 0;
    while (*tp && *tp != '"' && i < type_max - 1) {
        type_out[i++] = *tp++;
    }
    type_out[i] = '\0';

    /* Find "value" field */
    const char *vp = strstr(json, "\"value\"");
    if (vp) {
        vp = strchr(vp + 7, ':');
        if (vp) {
            vp++;
            while (*vp == ' ' || *vp == '\t') vp++;
            *value_out = atof(vp);
        }
    }

    return 1;
}

/* Escape a string for JSON embedding (minimal - just handle quotes and backslashes) */
static void json_escape_string(const char *src, char *dst, int dst_size) {
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
                dst[j++] = src[i];
            }
        } else if (src[i] == '\n') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
                dst[j++] = 'n';
            }
        } else if (src[i] == '\r') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
                dst[j++] = 'r';
            }
        } else if (src[i] == '\t') {
            if (j < dst_size - 3) {
                dst[j++] = '\\';
                dst[j++] = 't';
            }
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/* Add external subtitle ID to tracking list */
static void track_external_sub(PlayerInstance *p, int sub_id) {
    if (!p) return;
    if (p->external_sub_count >= p->external_sub_capacity) {
        int new_cap = p->external_sub_capacity == 0 ? 8 : p->external_sub_capacity * 2;
        int *new_ids = (int*)realloc(p->external_sub_ids, new_cap * sizeof(int));
        if (!new_ids) return;
        p->external_sub_ids = new_ids;
        p->external_sub_capacity = new_cap;
    }
    p->external_sub_ids[p->external_sub_count++] = sub_id;
}

/* ============================================================================
 * Track List Serialization
 * ============================================================================ */

static int build_tracks_json(PlayerInstance *p, const char *track_type, int include_forced, char *buf, int buf_size) {
    if (!p || !p->mpv) {
        snprintf(buf, buf_size, "[]");
        return 2;
    }

    mpv_node node;
    memset(&node, 0, sizeof(node));
    int err = g_libs.mpv_get_property(p->mpv, "track-list", MPV_FORMAT_NODE, &node);
    if (err < 0 || node.format != MPV_FORMAT_NODE_ARRAY || !node.u.list) {
        snprintf(buf, buf_size, "[]");
        return 2;
    }

    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "[");

    int track_index = 0;
    mpv_node_list *list = node.u.list;
    for (int i = 0; i < list->num && pos < buf_size - 100; i++) {
        if (list->values[i].format != MPV_FORMAT_NODE_MAP) continue;
        mpv_node_list *track = list->values[i].u.list;
        if (!track) continue;

        /* Extract track fields */
        const char *type = NULL;
        int64_t id = 0;
        const char *title = NULL;
        const char *lang = NULL;
        int selected = 0;
        int forced = 0;
        int external = 0;

        for (int k = 0; k < track->num; k++) {
            if (!track->keys[k]) continue;
            if (strcmp(track->keys[k], "type") == 0 && track->values[k].format == MPV_FORMAT_STRING) {
                type = track->values[k].u.string;
            } else if (strcmp(track->keys[k], "id") == 0 && track->values[k].format == MPV_FORMAT_INT64) {
                id = track->values[k].u.int64;
            } else if (strcmp(track->keys[k], "title") == 0 && track->values[k].format == MPV_FORMAT_STRING) {
                title = track->values[k].u.string;
            } else if (strcmp(track->keys[k], "lang") == 0 && track->values[k].format == MPV_FORMAT_STRING) {
                lang = track->values[k].u.string;
            } else if (strcmp(track->keys[k], "selected") == 0 && track->values[k].format == MPV_FORMAT_FLAG) {
                selected = track->values[k].u.flag;
            } else if (strcmp(track->keys[k], "forced") == 0 && track->values[k].format == MPV_FORMAT_FLAG) {
                forced = track->values[k].u.flag;
            } else if (strcmp(track->keys[k], "external") == 0 && track->values[k].format == MPV_FORMAT_FLAG) {
                external = track->values[k].u.flag;
            }
        }

        if (!type || strcmp(type, track_type) != 0) continue;

        char escaped_title[512] = "";
        char escaped_lang[128] = "";
        if (title) json_escape_string(title, escaped_title, sizeof(escaped_title));
        if (lang) json_escape_string(lang, escaped_lang, sizeof(escaped_lang));

        if (track_index > 0) {
            pos += snprintf(buf + pos, buf_size - pos, ",");
        }

        if (include_forced) {
            pos += snprintf(buf + pos, buf_size - pos,
                "{\"index\":%d,\"id\":\"%lld\",\"label\":\"%s\",\"language\":\"%s\",\"selected\":%s,\"forced\":%s}",
                track_index,
                (long long)id,
                escaped_title[0] ? escaped_title : (escaped_lang[0] ? escaped_lang : "Unknown"),
                escaped_lang,
                selected ? "true" : "false",
                forced ? "true" : "false");
        } else {
            pos += snprintf(buf + pos, buf_size - pos,
                "{\"index\":%d,\"id\":\"%lld\",\"label\":\"%s\",\"language\":\"%s\",\"selected\":%s}",
                track_index,
                (long long)id,
                escaped_title[0] ? escaped_title : (escaped_lang[0] ? escaped_lang : "Unknown"),
                escaped_lang,
                selected ? "true" : "false");
        }
        track_index++;
    }

    pos += snprintf(buf + pos, buf_size - pos, "]");
    g_libs.mpv_free_node_contents(&node);
    return pos;
}


/* ============================================================================
 * WebKit JS Bridge - Script Message Handler
 * ============================================================================ */

static void on_script_message(void *manager, void *result, gpointer data) {
    (void)manager;
    (void)data;
    PlayerInstance *p = (PlayerInstance*)g_current_player;
    if (!p) return;
    if (!p || !result) return;

    /* Step 1: WebKitJavascriptResult → JSCValue (from libwebkit2gtk-4.1.so) */
    void *js_value = g_libs.webkit_javascript_result_get_js_value(result);
    if (!js_value) return;

    /* Step 2: JSCValue → JSON string (from libjavascriptcoregtk-4.1.so) */
    char *json_str = g_libs.jsc_value_to_json(js_value, 0);
    if (!json_str) return;

    /* Parse the message */
    char type[128];
    double value = 0.0;
    if (!parse_message_json(json_str, type, sizeof(type), &value)) {
        g_libs.g_free(json_str);
        return;
    }
    g_libs.g_free(json_str);

    /* Dispatch based on type */
    if (strcmp(type, "controlsReady") == 0) {
        p->controls_ready = 1;
        /* Flush pending controls JSON */
        if (p->pending_controls_json && p->web_view) {
            int len = strlen(p->pending_controls_json);
            char *script = (char*)malloc(len + 256);
            if (script) {
                snprintf(script, len + 256,
                    "(function(){if(!window.playerControls)return;window.playerControls(%s);})()",
                    p->pending_controls_json);
                g_libs.webkit_web_view_run_javascript(p->web_view, script, NULL, NULL, NULL);
                free(script);
            }
            free(p->pending_controls_json);
            p->pending_controls_json = NULL;
        }
    } else if (strcmp(type, "selectAudioTrack") == 0) {
        if (p->mpv) {
            char id_str[32];
            snprintf(id_str, sizeof(id_str), "%d", (int)value);
            g_libs.mpv_set_property_string(p->mpv, "aid", id_str);
        }
    } else if (strcmp(type, "selectSubtitleTrack") == 0) {
        if (p->mpv) {
            int track_id = (int)value;
            if (track_id < 0) {
                g_libs.mpv_set_property_string(p->mpv, "sid", "no");
            } else {
                char id_str[32];
                snprintf(id_str, sizeof(id_str), "%d", track_id);
                g_libs.mpv_set_property_string(p->mpv, "sid", id_str);
            }
        }
    } else {
        /* Forward all other events to JNI event sink */
        send_player_event(p, type, value);
    }
}

/* ============================================================================
 * Native → JS Communication
 * ============================================================================ */

typedef struct {
    PlayerInstance *player;
    char *script;
} RunJsData;

static gboolean run_js_on_gtk_thread(gpointer data) {
    RunJsData *rj = (RunJsData*)data;
    if (rj && rj->player && rj->player->web_view && rj->player->running && rj->script) {
        g_libs.webkit_web_view_run_javascript(rj->player->web_view, rj->script, NULL, NULL, NULL);
    }
    if (rj) {
        free(rj->script);
        free(rj);
    }
    return 0; /* G_SOURCE_REMOVE */
}

static void dispatch_js(PlayerInstance *p, char *script) {
    if (!p || !script) {
        free(script);
        return;
    }
    RunJsData *rj = (RunJsData*)malloc(sizeof(RunJsData));
    if (!rj) {
        free(script);
        return;
    }
    rj->player = p;
    rj->script = script;
    g_libs.g_idle_add(run_js_on_gtk_thread, rj);
}

/* ============================================================================
 * Player Update Timer (500ms) - runs on GTK thread
 * ============================================================================ */

static gboolean player_update_timer(gpointer data) {
    PlayerInstance *p = (PlayerInstance*)data;
    if (!p || !p->running || !p->mpv) return 0;
    if (!p->web_view) return 0; /* No WebView — playerUpdate handled from Kotlin side */

    /* Query mpv state */
    double duration = 0.0;
    double position = 0.0;
    int paused = 0;
    int seeking = 0;
    int paused_for_cache = 0;
    int eof_reached = 0;

    g_libs.mpv_get_property(p->mpv, "duration", MPV_FORMAT_DOUBLE, &duration);
    g_libs.mpv_get_property(p->mpv, "time-pos", MPV_FORMAT_DOUBLE, &position);
    g_libs.mpv_get_property(p->mpv, "pause", MPV_FORMAT_FLAG, &paused);
    g_libs.mpv_get_property(p->mpv, "seeking", MPV_FORMAT_FLAG, &seeking);
    g_libs.mpv_get_property(p->mpv, "paused-for-cache", MPV_FORMAT_FLAG, &paused_for_cache);
    g_libs.mpv_get_property(p->mpv, "eof-reached", MPV_FORMAT_FLAG, &eof_reached);

    int loading = seeking || paused_for_cache;
    p->is_loading = loading;
    p->is_ended = eof_reached;

    if (duration < 0) duration = 0;
    if (position < 0) position = 0;

    /* Build audio/subtitle track JSON */
    char audio_tracks[8192];
    char sub_tracks[8192];
    build_tracks_json(p, "audio", 0, audio_tracks, sizeof(audio_tracks));
    build_tracks_json(p, "sub", 1, sub_tracks, sizeof(sub_tracks));

    /* Build playerUpdate script */
    int script_size = strlen(audio_tracks) + strlen(sub_tracks) + 512;
    char *script = (char*)malloc(script_size);
    if (script) {
        snprintf(script, script_size,
            "window.playerUpdate&&window.playerUpdate({duration:%.3f,position:%.3f,"
            "paused:%s,loading:%s,audioTracks:%s,subtitleTracks:%s})",
            duration, position,
            paused ? "true" : "false",
            loading ? "true" : "false",
            audio_tracks, sub_tracks);
        if (p->web_view) {
            g_libs.webkit_web_view_run_javascript(p->web_view, script, NULL, NULL, NULL);
        }
        free(script);
    }

    return p->running ? 1 : 0; /* G_SOURCE_CONTINUE or G_SOURCE_REMOVE */
}

/* ============================================================================
 * mpv Render Update Callback
 * ============================================================================ */

static void on_mpv_render_update(void *ctx) {
    PlayerInstance *p = (PlayerInstance*)ctx;
    p->render_needed = 1;
    pthread_mutex_lock(&p->render_mutex);
    pthread_cond_signal(&p->render_cond);
    pthread_mutex_unlock(&p->render_mutex);
}

/* ============================================================================
 * WebKit Overlay Creation (RGBA offscreen + webkit_web_view_get_snapshot)
 *
 * Single offscreen WebView with RGBA visual and transparent background.
 * Snapshots are taken via webkit_web_view_get_snapshot which produces
 * a fresh cairo surface with correct alpha on every call (no accumulation).
 * ============================================================================ */

/* "draw" signal handler for offscreen window — clears surface to fully transparent
 * before GTK/WebKit paints on it. Required when app_paintable=TRUE with RGBA visual,
 * otherwise the alpha channel retains stale values from previous frames. */
static gboolean on_offscreen_draw(void *widget, void *cr, void *data) {
    (void)widget; (void)data;
    /* cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE) + paint with alpha=0 */
    typedef void (*fn_cairo_set_operator)(void*, int);
    typedef void (*fn_cairo_set_source_rgba)(void*, double, double, double, double);
    typedef void (*fn_cairo_paint)(void*);
    static fn_cairo_set_operator set_op = NULL;
    static fn_cairo_set_source_rgba set_rgba = NULL;
    static fn_cairo_paint paint = NULL;
    static int resolved = 0;
    if (!resolved) {
        void *h = g_libs.cairo_lib ? g_libs.cairo_lib : dlopen("libcairo.so.2", RTLD_NOW);
        if (h) {
            set_op = (fn_cairo_set_operator)dlsym(h, "cairo_set_operator");
            set_rgba = (fn_cairo_set_source_rgba)dlsym(h, "cairo_set_source_rgba");
            paint = (fn_cairo_paint)dlsym(h, "cairo_paint");
        }
        resolved = 1;
    }
    if (set_op && set_rgba && paint && cr) {
        set_op(cr, 1); /* CAIRO_OPERATOR_SOURCE = 1 */
        set_rgba(cr, 0.0, 0.0, 0.0, 0.0); /* fully transparent */
        paint(cr);
    }
    return 0; /* FALSE — propagate to child widgets (WebKit draws after us) */
}

/* Callback for webkit_web_view_get_snapshot — copies snapshot pixels to staging buffer */
static void on_snapshot_ready(void *source_object, void *result, void *user_data) {
    (void)user_data;
    PlayerInstance *p = (PlayerInstance*)g_current_player;
    if (!p || !p->running) {
        g_snapshot_pending = 0;
        return;
    }

    typedef void* (*fn_webkit_get_snapshot_finish)(void*, void*, void**);
    static fn_webkit_get_snapshot_finish snap_finish = NULL;
    if (!snap_finish && g_libs.webkit_lib) {
        snap_finish = (fn_webkit_get_snapshot_finish)dlsym(g_libs.webkit_lib, "webkit_web_view_get_snapshot_finish");
    }
    if (!snap_finish) { g_snapshot_pending = 0; return; }

    void *error = NULL;
    void *cairo_surface = snap_finish(source_object, result, &error);
    if (!cairo_surface || error) {
        static int err_log = 0;
        if (err_log < 5) { fprintf(stderr, "[player_bridge] snapshot FAILED (error=%p surface=%p)\n", error, cairo_surface); err_log++; }
        if (error) {
            typedef void (*fn_g_error_free)(void*);
            fn_g_error_free err_free = (fn_g_error_free)dlsym(RTLD_DEFAULT, "g_error_free");
            if (err_free) err_free(error);
        }
        g_snapshot_pending = 0;
        return;
    }

    /* Extract pixels from the snapshot surface */
    if (g_libs.cairo_surface_flush && g_libs.cairo_image_surface_get_data &&
        g_libs.cairo_image_surface_get_width && g_libs.cairo_image_surface_get_height &&
        g_libs.cairo_image_surface_get_stride) {

        g_libs.cairo_surface_flush(cairo_surface);
        unsigned char *data = g_libs.cairo_image_surface_get_data(cairo_surface);
        int sw = g_libs.cairo_image_surface_get_width(cairo_surface);
        int sh = g_libs.cairo_image_surface_get_height(cairo_surface);
        int stride = g_libs.cairo_image_surface_get_stride(cairo_surface);

        if (data && sw > 0 && sh > 0) {
            int byte_count = sw * sh * 4;
            pthread_mutex_lock(&p->overlay_mutex);
            if (p->overlay_staging_width != sw || p->overlay_staging_height != sh) {
                free(p->overlay_staging);
                p->overlay_staging = (unsigned char*)malloc(byte_count);
                if (!p->overlay_staging) {
                    p->overlay_staging_width = 0;
                    p->overlay_staging_height = 0;
                    pthread_mutex_unlock(&p->overlay_mutex);
                    goto done;
                }
                p->overlay_staging_width = sw;
                p->overlay_staging_height = sh;
            }
            if (stride == sw * 4) {
                memcpy(p->overlay_staging, data, byte_count);
            } else {
                for (int row = 0; row < sh; row++) {
                    memcpy(p->overlay_staging + row * sw * 4, data + row * stride, sw * 4);
                }
            }
            p->overlay_dirty = 1;
            pthread_mutex_unlock(&p->overlay_mutex);

            pthread_mutex_lock(&p->render_mutex);
            p->render_needed = 1;
            pthread_cond_signal(&p->render_cond);
            pthread_mutex_unlock(&p->render_mutex);
        }
    }
    done:
    /* Free the snapshot surface */
    {
        typedef void (*fn_cairo_surface_destroy_t)(void*);
        static fn_cairo_surface_destroy_t surf_destroy = NULL;
        if (!surf_destroy) {
            void *ch = g_libs.cairo_lib ? g_libs.cairo_lib : dlopen("libcairo.so.2", RTLD_NOW);
            if (ch) surf_destroy = (fn_cairo_surface_destroy_t)dlsym(ch, "cairo_surface_destroy");
        }
        if (surf_destroy) surf_destroy(cairo_surface);
    }
    /* Mark snapshot as complete — allow next request */
    g_snapshot_pending = 0;
}

static void create_webkit_overlay_on_gtk_thread(PlayerInstance *p) {
    if (!p || !p->running) return;

    fprintf(stderr, "[player_bridge] create_webkit_overlay: starting (RGBA snapshot)\n");

    /* WebKit/GObject type functions needed for WebView creation */
    typedef unsigned long (*fn_webkit_web_view_get_type)(void);
    typedef void* (*fn_g_object_new_t)(unsigned long type, const char *first, ...);
    fn_webkit_web_view_get_type get_type = (fn_webkit_web_view_get_type)dlsym(RTLD_DEFAULT, "webkit_web_view_get_type");
    fn_g_object_new_t g_obj_new = (fn_g_object_new_t)dlsym(RTLD_DEFAULT, "g_object_new");

    /* Determine initial size from host window */
    int init_w = 1280, init_h = 720;
    {
        void *x11_lib_local = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
        if (!x11_lib_local) x11_lib_local = dlopen("libX11.so.6", RTLD_LAZY);
        if (x11_lib_local && p->host_window) {
            typedef void* (*fn_XOpenDisplay_local)(const char*);
            typedef int (*fn_XGetWindowAttributes_local)(void*, unsigned long, void*);
            fn_XGetWindowAttributes_local xga = (fn_XGetWindowAttributes_local)dlsym(x11_lib_local, "XGetWindowAttributes");
            fn_XOpenDisplay_local xod = (fn_XOpenDisplay_local)dlsym(x11_lib_local, "XOpenDisplay");
            if (xga && xod) {
                void *dpy = xod(NULL);
                if (dpy) {
                    int attrs[64]; memset(attrs, 0, sizeof(attrs));
                    if (xga(dpy, p->host_window, attrs)) {
                        if (attrs[2] > 0 && attrs[3] > 0) { init_w = attrs[2]; init_h = attrs[3]; }
                    }
                    typedef int (*fn_XCloseDisplay_local)(void*);
                    fn_XCloseDisplay_local xcd = (fn_XCloseDisplay_local)dlsym(x11_lib_local, "XCloseDisplay");
                    if (xcd) xcd(dpy);
                }
            }
        }
    }

    /* --- Offscreen window for WebKit snapshot rendering --- */
    /* Reuse existing to avoid WebKit2GTK crash on repeated WebView creation */
    if (!g_gtk_window_black) {
        g_gtk_window_black = g_libs.gtk_offscreen_window_new();
        if (!g_gtk_window_black) {
            fprintf(stderr, "[player_bridge] gtk_offscreen_window_new failed\n");
            return;
        }
        g_libs.gtk_window_set_default_size(g_gtk_window_black, init_w, init_h);

        /* Set RGBA visual for alpha transparency in snapshots */
        {
            void *screen = g_libs.gdk_screen_get_default ? g_libs.gdk_screen_get_default() : NULL;
            if (!screen && g_libs.gtk_widget_get_screen) {
                screen = g_libs.gtk_widget_get_screen(g_gtk_window_black);
            }
            if (screen) {
                void *rgba_visual = g_libs.gdk_screen_get_rgba_visual(screen);
                if (rgba_visual) {
                    g_libs.gtk_widget_set_visual(g_gtk_window_black, rgba_visual);
                    fprintf(stderr, "[player_bridge] RGBA visual SET on offscreen window\n");
                } else {
                    fprintf(stderr, "[player_bridge] WARNING: no RGBA visual available!\n");
                }
            }
        }
        g_content_manager_black = g_libs.webkit_user_content_manager_new();
        if (g_content_manager_black) {
            g_libs.webkit_user_content_manager_register_script_message_handler(g_content_manager_black, "player");
            g_libs.g_signal_connect_data(g_content_manager_black,
                "script-message-received::player",
                (GCallback)on_script_message, p, NULL, 0);
        }

        void *settings = g_libs.webkit_settings_new ? g_libs.webkit_settings_new() : NULL;
        if (settings) {
            g_libs.webkit_settings_set_hardware_acceleration_policy(settings, 2); /* NEVER */
        }

        if (egl_makeCurrent && p->egl_display) {
            egl_makeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        if (get_type && g_obj_new && settings && g_content_manager_black) {
            unsigned long wv_type = get_type();
            g_web_view_black = g_obj_new(wv_type, "settings", settings, "user-content-manager", g_content_manager_black, NULL);
        } else if (g_content_manager_black) {
            g_web_view_black = g_libs.webkit_web_view_new_with_user_content_manager(g_content_manager_black);
        }

        if (g_web_view_black) {
            /* Transparent background — requires RGBA visual on parent window */
            GdkRGBA transparent_bg = {0.0, 0.0, 0.0, 0.0};
            g_libs.webkit_web_view_set_background_color(g_web_view_black, &transparent_bg);
            g_libs.gtk_container_add(g_gtk_window_black, g_web_view_black);
        }
        g_libs.gtk_widget_show_all(g_gtk_window_black);
        fprintf(stderr, "[player_bridge] create_webkit_overlay: WebKit offscreen created\n");
    } else {
        /* Reuse — just resize */
        g_libs.gtk_window_resize(g_gtk_window_black, init_w, init_h);
        if (g_web_view_black) g_libs.gtk_widget_set_size_request(g_web_view_black, init_w, init_h);
        fprintf(stderr, "[player_bridge] create_webkit_overlay: WebKit offscreen reused\n");

        if (egl_makeCurrent && p->egl_display) {
            egl_makeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }

    /* Reset snapshot state for fresh start */
    g_snapshot_pending = 0;
    g_draw_capture_active = 0;
    g_overlay_input_dirty = 0;

    /* Reset GL overlay state — previous EGL context is destroyed,
     * GL resources (shader, VAO, texture) must be recreated. */
    p->overlay_initialized = 0;
    p->overlay_texture = 0;
    p->overlay_shader = 0;
    p->overlay_vao = 0;
    p->overlay_vbo = 0;

    /* Assign global views to player instance */
    p->gtk_window = g_gtk_window_black;
    p->web_view = g_web_view_black;
    p->content_manager = g_content_manager_black;
    g_current_player = p; /* Update active player for signal callbacks */

    /* Load (or reload) the controls page */
    if (p->web_view && p->controls_page_url) {
        p->controls_ready = 0; /* Reset — will be set again by "controlsReady" JS message */
        /* Use reload or load with different URL to force fresh page load.
         * WebKit may skip load_uri if URL is the same as current page. */
        g_libs.webkit_web_view_load_uri(p->web_view, "about:blank");
        /* Pump to process the blank load */
        for (int ri = 0; ri < 10 && g_libs.g_main_context_iteration(NULL, 0); ri++) {}
        g_libs.webkit_web_view_load_uri(p->web_view, p->controls_page_url);
    }

    /* Restore EGL context */
    if (egl_makeCurrent && p->egl_display && p->egl_surface && p->egl_context) {
        egl_makeCurrent(p->egl_display, p->egl_surface, p->egl_surface, p->egl_context);
    }
    fprintf(stderr, "[player_bridge] create_webkit_overlay: WebKit ready\n");

    /* Restore EGL context */
    if (egl_makeCurrent && p->egl_display && p->egl_surface && p->egl_context) {
        egl_makeCurrent(p->egl_display, p->egl_surface, p->egl_surface, p->egl_context);
    }

    /* Start the player update timer (500ms) */
    p->update_timer_id = g_libs.g_timeout_add(500, player_update_timer, p);

    fprintf(stderr, "[player_bridge] WebKit overlay ready (RGBA + get_snapshot)\n");
}

/* ============================================================================
 * GTK Thread Function
 * ============================================================================ */

/* No-op GLib log handler — silences GDK warnings about device=NULL in synthetic events */
static void gdk_log_noop(const char *domain, int level, const char *msg, void *data) {
    (void)domain; (void)level; (void)msg; (void)data;
}

static void* gtk_thread_func(void *arg) {
    PlayerInstance *p = (PlayerInstance*)arg;

    /* mpv requires LC_NUMERIC=C for proper float parsing */
    setlocale(LC_NUMERIC, "C");

    /* GTK was loaded early (initGtkEarly) so GDK types are registered.
     * Now we can safely load WebKitGTK and call gtk_init on this thread. */
    if (!load_gtk_libraries()) {
        fprintf(stderr, "[player_bridge] Failed to load GTK/WebKit libraries\n");
    }

    /* Suppress GDK warnings about device=NULL — harmless with our synthetic events */
    {
        typedef void (*log_func_t)(const char*, int, const char*, void*);
        typedef unsigned int (*fn_g_log_set_handler_t)(const char*, int, log_func_t, void*);
        fn_g_log_set_handler_t set_handler = (fn_g_log_set_handler_t)dlsym(RTLD_DEFAULT, "g_log_set_handler");
        if (set_handler) {
            set_handler("Gdk", 16 | 32, (log_func_t)gdk_log_noop, NULL);
        }
    }
    /* Call gtk_init_check on render thread — safe now that AWT is already connected to X11 */
    if (g_libs.gtk_init_check) {
        int argc2 = 0;
        if (!g_libs.gtk_init_check(&argc2, NULL)) {
            fprintf(stderr, "[player_bridge] gtk_init_check failed on render thread\n");
        } else {
            fprintf(stderr, "[player_bridge] gtk_init_check OK on render thread\n");
        }
    }

    /* Step 1: Load ONLY libgtk-3 and initialize GTK FIRST.
     * This must happen BEFORE loading libwebkit2gtk which has libgdk-3 as NEEDED
     * dependency — GDK constructor registers types that require GTK to be init'd first. */
    /* Force X11 backend — Skiko may have partially init'd Wayland GDK */
    setenv("GDK_BACKEND", "x11", 1);
    g_libs.gtk_lib = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libs.gtk_lib) {
        fprintf(stderr, "[player_bridge] Failed to load libgtk-3.so.0: %s\n", dlerror());
    } else {
        /* Resolve gtk_init_check immediately and call it */
        g_libs.gtk_init_check = (fn_gtk_init_check)dlsym(g_libs.gtk_lib, "gtk_init_check");
        if (g_libs.gtk_init_check) {
            int gtk_argc = 0;
            if (!g_libs.gtk_init_check(&gtk_argc, NULL)) {
                fprintf(stderr, "[player_bridge] gtk_init_check failed\n");
            } else {
                fprintf(stderr, "[player_bridge] gtk_init_check OK\n");
            }
        }
        /* Check type after gtk_init */
        typedef unsigned long (*fn_gtype_check)(const char*);
        fn_gtype_check gtype_fn2 = (fn_gtype_check)dlsym(RTLD_DEFAULT, "g_type_from_name");
        if (gtype_fn2) {
            unsigned long gtype = gtype_fn2("GdkDisplayManager");
            fprintf(stderr, "[player_bridge] GdkDisplayManager type AFTER gtk_init = %lu\n", gtype);
        }
    }

    /* GTK init already done in initGtkEarly() */

    /* Initialize render synchronization */
    pthread_mutex_init(&p->render_mutex, NULL);
    pthread_cond_init(&p->render_cond, NULL);
    p->render_needed = 0;

    /* Initialize overlay mutex */
    pthread_mutex_init(&p->overlay_mutex, NULL);
    p->overlay_dirty = 0;
    p->overlay_staging = NULL;
    p->overlay_staging_width = 0;
    p->overlay_staging_height = 0;

    /* ---- Open X11 display and load EGL library ---- */
    void *x11_lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11_lib) x11_lib = dlopen("libX11.so", RTLD_LAZY);
    void *x11_display = NULL;
    typedef void* (*fn_XOpenDisplay)(const char*);
    typedef int (*fn_XGetWindowAttributes)(void*, unsigned long, void*);
    fn_XGetWindowAttributes x11_get_attrs = NULL;

    if (x11_lib) {
        fn_XOpenDisplay xopen = (fn_XOpenDisplay)dlsym(x11_lib, "XOpenDisplay");
        x11_get_attrs = (fn_XGetWindowAttributes)dlsym(x11_lib, "XGetWindowAttributes");
        if (xopen) x11_display = xopen(NULL);
    }

    /* Load EGL library */
    p->egl_lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if (!p->egl_lib) p->egl_lib = dlopen("libEGL.so", RTLD_LAZY);
    if (!p->egl_lib) {
        fprintf(stderr, "[player_bridge] Failed to load libEGL\n");
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }

    /* Load EGL function pointers */
    egl_getDisplay = (fn_eglGetDisplay)dlsym(p->egl_lib, "eglGetDisplay");
    egl_initialize = (fn_eglInitialize)dlsym(p->egl_lib, "eglInitialize");
    egl_chooseConfig = (fn_eglChooseConfig)dlsym(p->egl_lib, "eglChooseConfig");
    egl_createWindowSurface = (fn_eglCreateWindowSurface)dlsym(p->egl_lib, "eglCreateWindowSurface");
    egl_createContext = (fn_eglCreateContext)dlsym(p->egl_lib, "eglCreateContext");
    egl_makeCurrent = (fn_eglMakeCurrent)dlsym(p->egl_lib, "eglMakeCurrent");
    egl_swapBuffers = (fn_eglSwapBuffers)dlsym(p->egl_lib, "eglSwapBuffers");
    egl_getProcAddress = (fn_eglGetProcAddress)dlsym(p->egl_lib, "eglGetProcAddress");
    egl_terminate = (fn_eglTerminate)dlsym(p->egl_lib, "eglTerminate");
    egl_destroySurface = (fn_eglDestroySurface)dlsym(p->egl_lib, "eglDestroySurface");
    egl_destroyContext = (fn_eglDestroyContext)dlsym(p->egl_lib, "eglDestroyContext");
    egl_swapInterval = (fn_eglSwapInterval)dlsym(p->egl_lib, "eglSwapInterval");
    egl_bindAPI = (fn_eglBindAPI)dlsym(p->egl_lib, "eglBindAPI");

    /* ---- Create EGL context using eglGetPlatformDisplay (X11 platform) ---- */
    
    /* Load eglGetPlatformDisplay from EGL library */
    typedef void* (*fn_eglGetPlatformDisplay)(unsigned int platform, void *native_display, const int *attrib_list);
    fn_eglGetPlatformDisplay egl_getPlatformDisplay = (fn_eglGetPlatformDisplay)dlsym(p->egl_lib, "eglGetPlatformDisplay");
    
    #define EGL_PLATFORM_X11_EXT 0x31D5
    
    if (egl_getPlatformDisplay && x11_display) {
        p->egl_display = egl_getPlatformDisplay(EGL_PLATFORM_X11_EXT, x11_display, NULL);
    }
    if (!p->egl_display || p->egl_display == EGL_NO_DISPLAY) {
        /* Fallback */
        p->egl_display = egl_getDisplay((EGLNativeDisplayType)x11_display);
    }
    if (!p->egl_display || p->egl_display == EGL_NO_DISPLAY) {
        p->egl_display = egl_getDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (!p->egl_display || p->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "[player_bridge] eglGetPlatformDisplay/eglGetDisplay failed\n");
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }

    EGLint egl_major, egl_minor;
    if (!egl_initialize(p->egl_display, &egl_major, &egl_minor)) {
        fprintf(stderr, "[player_bridge] eglInitialize failed\n");
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }
    fprintf(stderr, "[player_bridge] EGL initialized: version %d.%d (platform x11)\n", egl_major, egl_minor);

    /* Bind OpenGL API (not ES) */
    egl_bindAPI(EGL_OPENGL_API);

    /* Choose EGL config */
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLConfig egl_config;
    EGLint num_configs;
    if (!egl_chooseConfig(p->egl_display, config_attribs, &egl_config, 1, &num_configs) || num_configs == 0) {
        fprintf(stderr, "[player_bridge] eglChooseConfig failed\n");
        egl_terminate(p->egl_display);
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }
    fprintf(stderr, "[player_bridge] EGL config chosen (%d configs available)\n", num_configs);

    /* Create EGL window surface directly on Canvas host window (24-bit).
     * Overlay compositing happens in GL (alpha blend in shader), not X11 —
     * so we don't need a 32-bit window. */
    p->egl_surface = egl_createWindowSurface(p->egl_display, egl_config,
                                              (EGLNativeWindowType)p->host_window, NULL);
    if (!p->egl_surface || p->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "[player_bridge] eglCreateWindowSurface failed\n");
        egl_terminate(p->egl_display);
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }

    /* Create OpenGL 3.3 core context */
    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    p->egl_context = egl_createContext(p->egl_display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (!p->egl_context || p->egl_context == EGL_NO_CONTEXT) {
        EGLint fallback_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
        p->egl_context = egl_createContext(p->egl_display, egl_config, EGL_NO_CONTEXT, fallback_attribs);
    }
    if (!p->egl_context || p->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "[player_bridge] eglCreateContext failed\n");
        egl_destroySurface(p->egl_display, p->egl_surface);
        egl_terminate(p->egl_display);
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }

    if (!egl_makeCurrent(p->egl_display, p->egl_surface, p->egl_surface, p->egl_context)) {
        fprintf(stderr, "[player_bridge] eglMakeCurrent failed\n");
        egl_destroyContext(p->egl_display, p->egl_context);
        egl_destroySurface(p->egl_display, p->egl_surface);
        egl_terminate(p->egl_display);
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }

    if (egl_swapInterval) egl_swapInterval(p->egl_display, 0);
    fprintf(stderr, "[player_bridge] EGL context ready on window 0x%lx\n", p->host_window);

    /* Load GL function pointers for overlay compositing */
    load_gl_functions();

    /* ---- Initialize mpv with vo=libmpv (render API) ---- */
    p->mpv = g_libs.mpv_create();
    if (!p->mpv) {
        fprintf(stderr, "[player_bridge] mpv_create failed\n");
        egl_destroyContext(p->egl_display, p->egl_context);
        egl_destroySurface(p->egl_display, p->egl_surface);
        egl_terminate(p->egl_display);
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }

    g_libs.mpv_set_option_string(p->mpv, "vo", "libmpv");
    g_libs.mpv_set_option_string(p->mpv, "hwdec", "auto");
    g_libs.mpv_set_option_string(p->mpv, "terminal", "no");
    g_libs.mpv_set_option_string(p->mpv, "msg-level", "all=no");
    g_libs.mpv_set_option_string(p->mpv, "keep-open", "yes");
    g_libs.mpv_set_option_string(p->mpv, "idle", "yes");

    int mpv_err = g_libs.mpv_initialize(p->mpv);
    if (mpv_err < 0) {
        fprintf(stderr, "[player_bridge] mpv_initialize failed: %s\n", g_libs.mpv_error_string(mpv_err));
        g_libs.mpv_destroy(p->mpv);
        p->mpv = NULL;
        egl_destroyContext(p->egl_display, p->egl_context);
        egl_destroySurface(p->egl_display, p->egl_surface);
        egl_terminate(p->egl_display);
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }

    /* Create mpv render context */
    mpv_opengl_init_params gl_init = { .get_proc_address = get_gl_proc_address, .get_proc_address_ctx = NULL };
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_INVALID, NULL}
    };
    int rc = g_libs.mpv_render_context_create(&p->render_ctx, p->mpv, render_params);
    if (rc < 0) {
        fprintf(stderr, "[player_bridge] mpv_render_context_create failed: %s\n", g_libs.mpv_error_string(rc));
        g_libs.mpv_destroy(p->mpv);
        p->mpv = NULL;
        egl_destroyContext(p->egl_display, p->egl_context);
        egl_destroySurface(p->egl_display, p->egl_surface);
        egl_terminate(p->egl_display);
        pthread_mutex_lock(&p->init_mutex);
        p->init_done = -1;
        pthread_cond_signal(&p->init_cond);
        pthread_mutex_unlock(&p->init_mutex);
        return NULL;
    }
    g_libs.mpv_render_context_set_update_callback(p->render_ctx, on_mpv_render_update, p);
    fprintf(stderr, "[player_bridge] mpv render context created (libmpv + EGL x11)\n");

    /* Observe properties */
    g_libs.mpv_observe_property(p->mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    g_libs.mpv_observe_property(p->mpv, 0, "seeking", MPV_FORMAT_FLAG);
    g_libs.mpv_observe_property(p->mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);

    /* ---- Load media ---- */
    if (p->source_url && p->mpv) {
        if (p->header_lines && p->header_count > 0) {
            mpv_node node;
            mpv_node_list node_list;
            mpv_node *items = (mpv_node*)calloc(p->header_count, sizeof(mpv_node));
            if (items) {
                node_list.num = p->header_count;
                node_list.values = items;
                node_list.keys = NULL;
                for (int i = 0; i < p->header_count; i++) {
                    items[i].format = MPV_FORMAT_STRING;
                    items[i].u.string = p->header_lines[i];
                }
                node.format = MPV_FORMAT_NODE_ARRAY;
                node.u.list = &node_list;
                g_libs.mpv_set_property(p->mpv, "http-header-fields", MPV_FORMAT_NODE, &node);
                free(items);
            }
        }
        if (p->initial_position_ms > 0) {
            char start_str[64];
            snprintf(start_str, sizeof(start_str), "%.3f", p->initial_position_ms / 1000.0);
            g_libs.mpv_set_option_string(p->mpv, "start", start_str);
        }
        int pause_flag = p->play_when_ready ? 0 : 1;
        g_libs.mpv_set_property(p->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);
        const char *cmd[] = {"loadfile", p->source_url, NULL};
        g_libs.mpv_command(p->mpv, cmd);
    }

    /* ---- Signal init complete ---- */
    pthread_mutex_lock(&p->init_mutex);
    p->init_done = 1;
    pthread_cond_signal(&p->init_cond);
    pthread_mutex_unlock(&p->init_mutex);

    /* ---- Create WebKit offscreen overlay (in-process, GTK already init'd) ---- */
    create_webkit_overlay_on_gtk_thread(p);

    /* ---- Render loop ---- */
    int last_overlay_w = 1280, last_overlay_h = 720;

    /* X11 mouse polling for input forwarding to offscreen WebKit */
    typedef int (*fn_XQueryPointer_t)(void*, unsigned long, unsigned long*, unsigned long*,
                                      int*, int*, int*, int*, unsigned int*);
    fn_XQueryPointer_t x11_query_pointer = NULL;
    if (x11_lib) x11_query_pointer = (fn_XQueryPointer_t)dlsym(x11_lib, "XQueryPointer");
    int last_mouse_x = -1, last_mouse_y = -1;
    unsigned int last_mouse_buttons = 0;
    int mouse_poll_counter = 0;

    while (p->running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        /* Always target ~60fps (16ms). Don't reduce to 2ms — that burns CPU
         * unnecessarily. The overlay renders at 30fps but video at 60fps. */
        ts.tv_nsec += 16000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        
        pthread_mutex_lock(&p->render_mutex);
        if (!p->render_needed) {
            pthread_cond_timedwait(&p->render_cond, &p->render_mutex, &ts);
        }
        int should_render = p->render_needed;
        p->render_needed = 0;
        pthread_mutex_unlock(&p->render_mutex);

        /* Always render when overlay has new pixels — don't wait for mpv */
        if (!should_render && p->overlay_dirty && p->overlay_staging &&
            p->render_ctx && p->egl_display && p->egl_surface) {
            should_render = 1;
        }
        
        if (should_render && p->render_ctx && p->egl_display && p->egl_surface) {
            int w = 1280, h = 720;
            if (x11_get_attrs && x11_display && p->host_window) {
                int ab[64]; memset(ab, 0, sizeof(ab));
                if (x11_get_attrs(x11_display, p->host_window, ab)) {
                    if (ab[2] > 0 && ab[3] > 0) { w = ab[2]; h = ab[3]; }
                }
            }
            mpv_opengl_fbo fbo = { .fbo = 0, .w = w, .h = h, .internal_format = 0 };
            int flip_y = 1;
            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
                {MPV_RENDER_PARAM_INVALID, NULL}
            };
            g_libs.mpv_render_context_render(p->render_ctx, rp);
            
            /* ---- Overlay compositing ---- */
            if (p->overlay_dirty && p->overlay_staging) {
                if (!p->overlay_initialized) {
                    init_overlay_gl(p);
                }
                if (p->overlay_initialized) {
                    pthread_mutex_lock(&p->overlay_mutex);
                    /* Upload overlay texture */
                    if (p->overlay_staging && p->overlay_staging_width > 0 && p->overlay_staging_height > 0) {
                        if (!p->overlay_texture) gl_GenTextures(1, &p->overlay_texture);
                        gl_BindTexture(GL_TEXTURE_2D, p->overlay_texture);
                        gl_PixelStorei(0x0CF5, 1);
                        gl_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                                      p->overlay_staging_width, p->overlay_staging_height,
                                      0, GL_BGRA, GL_UNSIGNED_BYTE, p->overlay_staging);
                        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    }
                    p->overlay_dirty = 0;
                    pthread_mutex_unlock(&p->overlay_mutex);
                    render_overlay(p, w, h);
                }
            } else if (p->overlay_initialized && p->overlay_texture) {
                render_overlay(p, w, h);
            }
            
            egl_swapBuffers(p->egl_display, p->egl_surface);
        }
        
        /* Process mpv events */
        while (p->mpv) {
            mpv_event *event = g_libs.mpv_wait_event(p->mpv, 0);
            if (!event || event->event_id == MPV_EVENT_NONE) break;
            if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                mpv_event_property *prop = (mpv_event_property*)event->data;
                if (prop && prop->name) {
                    if (strcmp(prop->name, "eof-reached") == 0 && prop->format == MPV_FORMAT_FLAG)
                        p->is_ended = *(int*)prop->data;
                    else if (strcmp(prop->name, "seeking") == 0 && prop->format == MPV_FORMAT_FLAG)
                        p->is_loading = *(int*)prop->data;
                    else if (strcmp(prop->name, "paused-for-cache") == 0 && prop->format == MPV_FORMAT_FLAG)
                        if (*(int*)prop->data) p->is_loading = 1;
                }
            } else if (event->event_id == MPV_EVENT_SHUTDOWN) {
                p->running = 0; break;
            }
        }
        
        /* ---- Pump GTK/WebKit events (non-blocking, LIMITED) ----
         * Keep this fast — max 5 iterations. Snapshot callbacks and mouse
         * events are dispatched here, but we must not block mpv rendering. */
        if (p->running && g_libs.g_main_context_iteration) {
            for (int gtk_i = 0; gtk_i < 5 && g_libs.g_main_context_iteration(NULL, 0); gtk_i++) {}
        }

        /* ---- Resize offscreen WebKit windows to match host viewport ---- */
        if (p->running && p->gtk_window && x11_get_attrs && x11_display && p->host_window) {
            int cur_w = 1280, cur_h = 720;
            int ab2[64]; memset(ab2, 0, sizeof(ab2));
            if (x11_get_attrs(x11_display, p->host_window, ab2)) {
                if (ab2[2] > 0 && ab2[3] > 0) { cur_w = ab2[2]; cur_h = ab2[3]; }
            }
            if (cur_w != last_overlay_w || cur_h != last_overlay_h) {
                last_overlay_w = cur_w;
                last_overlay_h = cur_h;
                g_libs.gtk_window_resize(p->gtk_window, cur_w, cur_h);
                if (p->web_view) g_libs.gtk_widget_set_size_request(p->web_view, cur_w, cur_h);
                /* Notify JS of viewport size change */
                char resize_script[256];
                snprintf(resize_script, sizeof(resize_script),
                    "window.dispatchEvent&&window.dispatchEvent(new Event('resize'))");
                if (p->web_view)
                    g_libs.webkit_web_view_run_javascript(p->web_view, resize_script, NULL, NULL, NULL);
            }
        }

        /* ---- Forward mouse events to WebKit via GDK events (gives real CSS :hover) ---- */
        mouse_poll_counter++;
        if (mouse_poll_counter >= 1 && p->running && p->web_view &&
            x11_query_pointer && x11_display && p->host_window) {
            mouse_poll_counter = 0;
            unsigned long root_ret, child_ret;
            int root_x, root_y, win_x, win_y;
            unsigned int mask_ret = 0;
            if (x11_query_pointer(x11_display, p->host_window, &root_ret, &child_ret,
                                  &root_x, &root_y, &win_x, &win_y, &mask_ret)) {
                /* Scale coordinates from host window to WebKit viewport */
                int host_w = 1280, host_h = 720;
                if (x11_get_attrs && x11_display) {
                    int hab[64]; memset(hab, 0, sizeof(hab));
                    if (x11_get_attrs(x11_display, p->host_window, hab)) {
                        if (hab[2] > 0 && hab[3] > 0) { host_w = hab[2]; host_h = hab[3]; }
                    }
                }
                int wk_x = (last_overlay_w > 0 && host_w > 0) ? win_x * last_overlay_w / host_w : win_x;
                int wk_y = (last_overlay_h > 0 && host_h > 0) ? win_y * last_overlay_h / host_h : win_y;

                int mouse_moved = (win_x != last_mouse_x || win_y != last_mouse_y);
                unsigned int btn1 = (mask_ret >> 8) & 1;
                unsigned int prev_btn1 = (last_mouse_buttons >> 8) & 1;
                int button_pressed = (btn1 && !prev_btn1);
                int button_released = (!btn1 && prev_btn1);

                /* Use proper GDK event API (gdk_event_new + gdk_event_set_device).
                 * gdk_event_new allocates the event, we fill fields, then pass to widget_event.
                 * IMPORTANT: Do NOT call gdk_event_free — it unrefs the window and crashes.
                 * Instead we just free() the allocated memory after use, or better: don't free
                 * at all since gtk_widget_event doesn't take ownership.
                 * Actually: use gdk_event_put or just pass stack-allocated event after
                 * calling gdk_event_set_device on it via a temporary gdk_event_new. 
                 *
                 * Approach: allocate with gdk_event_new (sets up GType internals),
                 * set fields, set device, call widget_event, then carefully free.
                 * The crash was because gdk_event_free unrefs window. Fix: ref window first. */
                typedef void* (*fn_gdk_event_new_t)(int type);
                typedef void (*fn_gdk_event_free_t)(void *event);
                typedef void (*fn_gdk_event_set_device_t)(void *event, void *device);
                typedef void* (*fn_g_object_ref_t)(void *object);
                typedef void (*fn_gtk_widget_event_t)(void*, void*);
                static fn_gdk_event_new_t gdk_event_new_fn = NULL;
                static fn_gdk_event_free_t gdk_event_free_fn = NULL;
                static fn_gdk_event_set_device_t gdk_event_set_device_fn = NULL;
                static fn_g_object_ref_t g_object_ref_fn = NULL;
                static fn_gtk_widget_event_t widget_event = NULL;
                static int gdk_api_resolved = 0;
                if (!gdk_api_resolved) {
                    gdk_api_resolved = 1;
                    widget_event = (fn_gtk_widget_event_t)dlsym(RTLD_DEFAULT, "gtk_widget_event");
                    gdk_event_new_fn = (fn_gdk_event_new_t)dlsym(RTLD_DEFAULT, "gdk_event_new");
                    gdk_event_free_fn = (fn_gdk_event_free_t)dlsym(RTLD_DEFAULT, "gdk_event_free");
                    gdk_event_set_device_fn = (fn_gdk_event_set_device_t)dlsym(RTLD_DEFAULT, "gdk_event_set_device");
                    g_object_ref_fn = (fn_g_object_ref_t)dlsym(RTLD_DEFAULT, "g_object_ref");
                }

                /* Get the real GdkDevice (pointer) */
                static void *gdk_pointer_device = NULL;
                static int device_resolved = 0;
                if (!device_resolved) {
                    device_resolved = 1;
                    typedef void* (*fn_gdk_display_get_default_seat_t)(void*);
                    typedef void* (*fn_gdk_seat_get_pointer_t)(void*);
                    fn_gdk_display_get_default_seat_t get_seat =
                        (fn_gdk_display_get_default_seat_t)dlsym(RTLD_DEFAULT, "gdk_display_get_default_seat");
                    fn_gdk_seat_get_pointer_t get_pointer =
                        (fn_gdk_seat_get_pointer_t)dlsym(RTLD_DEFAULT, "gdk_seat_get_pointer");
                    if (get_seat && get_pointer && g_libs.gdk_display_get_default) {
                        void *display = g_libs.gdk_display_get_default();
                        if (display) {
                            void *seat = get_seat(display);
                            if (seat) {
                                gdk_pointer_device = get_pointer(seat);
                            }
                        }
                    }
                }

                void *gdk_window = g_libs.gtk_widget_get_window(p->web_view);

                if (widget_event && gdk_window && gdk_event_new_fn && gdk_event_free_fn) {
                    /* Helper: GdkEventMotion field offsets (from gdk_event_new allocated struct).
                     * We cast to our known layout to fill fields. gdk_event_new zeros the struct
                     * and sets up the GType header, so casting and filling fields is safe. */
                    typedef struct { int type; void *window; char send_event; unsigned int time;
                        double x, y; double *axes; unsigned int state; short is_hint;
                        void *device; double x_root, y_root; } MotionEv;
                    typedef struct { int type; void *window; char send_event; unsigned int time;
                        double x, y; double *axes; unsigned int state; unsigned int button;
                        void *device; double x_root, y_root; } ButtonEv;

                    if (mouse_moved) {
                        void *ev = gdk_event_new_fn(3); /* GDK_MOTION_NOTIFY */
                        if (ev) {
                            MotionEv *mev = (MotionEv*)ev;
                            /* Ref window before assigning — gdk_event_free will unref it */
                            if (g_object_ref_fn) g_object_ref_fn(gdk_window);
                            mev->window = gdk_window;
                            mev->send_event = 1;
                            mev->x = (double)wk_x; mev->y = (double)wk_y;
                            mev->x_root = (double)root_x; mev->y_root = (double)root_y;
                            mev->state = mask_ret;
                            if (gdk_event_set_device_fn && gdk_pointer_device)
                                gdk_event_set_device_fn(ev, gdk_pointer_device);
                            widget_event(p->web_view, ev);
                            gdk_event_free_fn(ev);
                        }
                    }
                    if (button_pressed) {
                        void *ev = gdk_event_new_fn(4); /* GDK_BUTTON_PRESS */
                        if (ev) {
                            ButtonEv *bev = (ButtonEv*)ev;
                            if (g_object_ref_fn) g_object_ref_fn(gdk_window);
                            bev->window = gdk_window;
                            bev->send_event = 1;
                            bev->x = (double)wk_x; bev->y = (double)wk_y;
                            bev->x_root = (double)root_x; bev->y_root = (double)root_y;
                            bev->button = 1; bev->state = mask_ret;
                            if (gdk_event_set_device_fn && gdk_pointer_device)
                                gdk_event_set_device_fn(ev, gdk_pointer_device);
                            widget_event(p->web_view, ev);
                            gdk_event_free_fn(ev);
                        }
                    }
                    if (button_released) {
                        void *ev = gdk_event_new_fn(7); /* GDK_BUTTON_RELEASE */
                        if (ev) {
                            ButtonEv *bev = (ButtonEv*)ev;
                            if (g_object_ref_fn) g_object_ref_fn(gdk_window);
                            bev->window = gdk_window;
                            bev->send_event = 1;
                            bev->x = (double)wk_x; bev->y = (double)wk_y;
                            bev->x_root = (double)root_x; bev->y_root = (double)root_y;
                            bev->button = 1; bev->state = mask_ret;
                            if (gdk_event_set_device_fn && gdk_pointer_device)
                                gdk_event_set_device_fn(ev, gdk_pointer_device);
                            widget_event(p->web_view, ev);
                            gdk_event_free_fn(ev);
                        }
                    }
                }

                /* Also dispatch JS events for range inputs (seek bar) which need
                 * programmatic value setting that GDK events alone don't provide */
                if (button_pressed && p->web_view) {
                    char js[512];
                    snprintf(js, sizeof(js),
                        "(function(){"
                        "var e=document.elementFromPoint(%d,%d);"
                        "if(e&&e.tagName==='INPUT'&&e.type==='range'){"
                        "var r=e.getBoundingClientRect();"
                        "var pct=Math.max(0,Math.min(1,(%d-r.left)/r.width));"
                        "e.value=Number(e.min)+(Number(e.max)-Number(e.min))*pct;"
                        "e.dispatchEvent(new Event('input',{bubbles:true}));"
                        "e.dispatchEvent(new Event('change',{bubbles:true}));"
                        "}"
                        "})()", wk_x, wk_y, wk_x);
                    g_libs.webkit_web_view_run_javascript(p->web_view, js, NULL, NULL, NULL);
                }

                last_mouse_x = win_x;
                last_mouse_y = win_y;
                last_mouse_buttons = mask_ret;

                /* Extra GTK pump after forwarding mouse events */
                if (mouse_moved || button_pressed || button_released) {
                    g_overlay_input_dirty = 1;
                    for (int pi = 0; pi < 5 && g_libs.g_main_context_iteration(NULL, 0); pi++) {}
                }
            }
        }

        /* ---- Capture WebKit overlay via async snapshot API ---- */
        if (!g_draw_capture_active) {
            typedef void (*fn_webkit_get_snapshot)(void*, int, int, void*, void*, void*);
            static fn_webkit_get_snapshot wk_snap = NULL;
            static int snap_api_resolved = 0;

            if (!snap_api_resolved) {
                if (g_libs.webkit_lib) {
                    wk_snap = (fn_webkit_get_snapshot)dlsym(g_libs.webkit_lib, "webkit_web_view_get_snapshot");
                }
                snap_api_resolved = 1;
            }

            static int snap_request_frame = 0;
            snap_request_frame++;
            if (g_snapshot_pending && snap_request_frame > 180) {
                g_snapshot_pending = 0;
            }

            if (wk_snap && p->web_view && p->controls_ready && !g_snapshot_pending) {
                g_snapshot_pending = 1;
                snap_request_frame = 0;
                g_overlay_input_dirty = 0;
                wk_snap(p->web_view, 1, 0, NULL, (void*)on_snapshot_ready, p);
            }
        }
    }
    
    /* Cleanup */
    /* Remove update timer if running */
    if (p->update_timer_id) {
        g_libs.g_source_remove(p->update_timer_id);
        p->update_timer_id = 0;
    }
    /* DO NOT destroy WebKit/GTK widgets — WebKit2GTK crashes when creating new
     * WebViews after destroying old ones with hw_accel=NEVER. They persist as globals. */
    if (p->web_view) {
        g_libs.webkit_web_view_load_uri(p->web_view, "about:blank");
    }
    p->web_view = NULL;
    p->gtk_window = NULL;
    p->content_manager = NULL;
    /* Persistent WebKit views handled above — not destroyed */
    /* Overlay GL resources */
    cleanup_overlay_gl(p);
    if (p->overlay_staging) { free(p->overlay_staging); p->overlay_staging = NULL; }
    pthread_mutex_destroy(&p->overlay_mutex);
    if (p->render_ctx) { g_libs.mpv_render_context_free(p->render_ctx); p->render_ctx = NULL; }
    if (p->mpv) { g_libs.mpv_destroy(p->mpv); p->mpv = NULL; }
    if (p->egl_display) {
        egl_makeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (p->egl_surface) { egl_destroySurface(p->egl_display, p->egl_surface); p->egl_surface = NULL; }
        if (p->egl_context) { egl_destroyContext(p->egl_display, p->egl_context); p->egl_context = NULL; }
        egl_terminate(p->egl_display); p->egl_display = NULL;
    }
    if (x11_display && x11_lib) {
        typedef int (*fn_XCloseDisplay)(void*);
        fn_XCloseDisplay xc = (fn_XCloseDisplay)dlsym(x11_lib, "XCloseDisplay");
        if (xc) xc(x11_display);
    }
    pthread_mutex_destroy(&p->render_mutex);
    pthread_cond_destroy(&p->render_cond);
    return NULL;
}


/* ============================================================================
 * Dispose Helper (called on GTK thread via g_idle_add)
 * ============================================================================ */

typedef struct {
    PlayerInstance *player;
    int *done_flag;
    pthread_mutex_t *done_mutex;
    pthread_cond_t *done_cond;
} DisposeData;

static gboolean dispose_on_gtk_thread(gpointer data) {
    DisposeData *dd = (DisposeData*)data;
    if (!dd) return 0;

    PlayerInstance *p = dd->player;
    if (p) {
        p->running = 0;
    }

    pthread_mutex_lock(dd->done_mutex);
    *dd->done_flag = 1;
    pthread_cond_signal(dd->done_cond);
    pthread_mutex_unlock(dd->done_mutex);

    return 0;
}

/* ============================================================================
 * JNI Entry Points
 * ============================================================================ */

#define JNI_PREFIX Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_

/* ---------- create ---------- */
JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_create(
    JNIEnv *env,
    jobject bridge,
    jlong hostViewPtr,
    jstring sourceUrl,
    jobjectArray headerLines,
    jboolean playWhenReady,
    jlong initialPositionMs,
    jstring controlsPageUrl,
    jint decoderPriority,
    jboolean nvidiaRtxSuperResolutionEnabled,
    jobject eventSink)
{
    (void)bridge;
    (void)nvidiaRtxSuperResolutionEnabled;

    /* Load libraries if not already loaded */
    if (!load_libraries()) {
        return 0;
    }

    /* Allocate player instance */
    PlayerInstance *p = (PlayerInstance*)calloc(1, sizeof(PlayerInstance));
    if (!p) return 0;

    /* Store JVM reference */
    (*env)->GetJavaVM(env, &p->jvm);

    /* Create global ref for event sink */
    if (eventSink) {
        p->event_sink = (*env)->NewGlobalRef(env, eventSink);
        jclass cls = (*env)->GetObjectClass(env, eventSink);
        if (cls) {
            p->event_method = (*env)->GetMethodID(env, cls, "onPlayerEvent", "(Ljava/lang/String;D)V");
            (*env)->DeleteLocalRef(env, cls);
        }
    }

    /* Store host window */
    p->host_window = (unsigned long)hostViewPtr;

    /* Copy source URL */
    if (sourceUrl) {
        const char *url_chars = (*env)->GetStringUTFChars(env, sourceUrl, NULL);
        if (url_chars) {
            p->source_url = strdup(url_chars);
            (*env)->ReleaseStringUTFChars(env, sourceUrl, url_chars);
        }
    }

    /* Copy header lines */
    if (headerLines) {
        int count = (*env)->GetArrayLength(env, headerLines);
        if (count > 0) {
            p->header_lines = (char**)calloc(count, sizeof(char*));
            if (p->header_lines) {
                p->header_count = count;
                for (int i = 0; i < count; i++) {
                    jstring jstr = (jstring)(*env)->GetObjectArrayElement(env, headerLines, i);
                    if (jstr) {
                        const char *chars = (*env)->GetStringUTFChars(env, jstr, NULL);
                        if (chars) {
                            p->header_lines[i] = strdup(chars);
                            (*env)->ReleaseStringUTFChars(env, jstr, chars);
                        }
                        (*env)->DeleteLocalRef(env, jstr);
                    }
                }
            }
        }
    }

    /* Copy controls page URL */
    if (controlsPageUrl) {
        const char *url_chars = (*env)->GetStringUTFChars(env, controlsPageUrl, NULL);
        if (url_chars) {
            p->controls_page_url = strdup(url_chars);
            (*env)->ReleaseStringUTFChars(env, controlsPageUrl, url_chars);
        }
    }

    p->play_when_ready = playWhenReady ? 1 : 0;
    p->initial_position_ms = (long long)initialPositionMs;
    p->decoder_priority = (int)decoderPriority;
    p->running = 1;

    /* Initialize synchronization */
    pthread_mutex_init(&p->init_mutex, NULL);
    pthread_cond_init(&p->init_cond, NULL);
    p->init_done = 0;

    /* Spawn GTK thread */
    if (pthread_create(&p->gtk_thread, NULL, gtk_thread_func, p) != 0) {
        fprintf(stderr, "[player_bridge] Failed to create GTK thread\n");
        if (p->event_sink) (*env)->DeleteGlobalRef(env, p->event_sink);
        free(p->source_url);
        free(p->controls_page_url);
        if (p->header_lines) {
            for (int i = 0; i < p->header_count; i++) free(p->header_lines[i]);
            free(p->header_lines);
        }
        pthread_mutex_destroy(&p->init_mutex);
        pthread_cond_destroy(&p->init_cond);
        free(p);
        return 0;
    }

    /* Wait for GTK thread to finish initialization */
    pthread_mutex_lock(&p->init_mutex);
    while (p->init_done == 0) {
        pthread_cond_wait(&p->init_cond, &p->init_mutex);
    }
    int init_result = p->init_done;
    pthread_mutex_unlock(&p->init_mutex);

    if (init_result < 0) {
        /* Initialization failed, clean up */
        p->running = 0;
        pthread_join(p->gtk_thread, NULL);
        if (p->event_sink) (*env)->DeleteGlobalRef(env, p->event_sink);
        free(p->source_url);
        free(p->controls_page_url);
        if (p->header_lines) {
            for (int i = 0; i < p->header_count; i++) free(p->header_lines[i]);
            free(p->header_lines);
        }
        pthread_mutex_destroy(&p->init_mutex);
        pthread_cond_destroy(&p->init_cond);
        free(p);
        return 0;
    }

    return (jlong)(intptr_t)p;
}

/* ---------- dispose ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_dispose(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->running && !p->mpv) return; /* Already disposed */

    /* Mark as not running — render loop will exit */
    p->running = 0;

    /* Wake up render thread if waiting on cond */
    pthread_mutex_lock(&p->render_mutex);
    p->render_needed = 1;
    pthread_cond_signal(&p->render_cond);
    pthread_mutex_unlock(&p->render_mutex);

    /* Wait for render thread to fully exit (it does all mpv/GL/GTK cleanup) */
    pthread_join(p->gtk_thread, NULL);

    /* WebKit/GTK widgets destroyed in gtk_thread_func cleanup */
    p->web_view = NULL;
    p->gtk_window = NULL;
    p->content_manager = NULL;

    /* Release JNI global ref */
    if (p->event_sink) {
        (*env)->DeleteGlobalRef(env, p->event_sink);
        p->event_sink = NULL;
    }

    /* Free pending controls */
    if (p->pending_controls_json) {
        free(p->pending_controls_json);
        p->pending_controls_json = NULL;
    }

    /* Free source data */
    free(p->source_url);
    free(p->controls_page_url);
    if (p->header_lines) {
        for (int i = 0; i < p->header_count; i++) {
            free(p->header_lines[i]);
        }
        free(p->header_lines);
    }

    /* Free external sub tracking */
    free(p->external_sub_ids);

    /* Destroy synchronization primitives */
    pthread_mutex_destroy(&p->init_mutex);
    pthread_cond_destroy(&p->init_cond);

    /* Free instance */
    free(p);
}

/* ---------- updateControls ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_updateControls(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jstring controlsJson)
{
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!controlsJson) return;

    const char *json = (*env)->GetStringUTFChars(env, controlsJson, NULL);
    if (!json) return;

    /* Track controls visibility — skip snapshots when UI is hidden */
    const char *vis_key = strstr(json, "\"controlsVisible\"");
    if (vis_key) {
        const char *val = vis_key + 17; /* skip "controlsVisible" */
        while (*val == ':' || *val == ' ' || *val == '\t') val++;
        p->controls_visible = (*val == 't' || *val == '1') ? 1 : 0;
    }

    if (p->controls_ready && p->web_view) {
        int len = strlen(json);
        char *script = (char*)malloc(len + 256);
        if (script) {
            snprintf(script, len + 256,
                "(function(){if(!window.playerControls)return;window.playerControls(%s);})()",
                json);
            dispatch_js(p, script);
            /* dispatch_js takes ownership of script */
        }
    } else {
        /* Store for later flush (controls dispatched when WebView is ready) */
        free(p->pending_controls_json);
        p->pending_controls_json = strdup(json);
    }

    (*env)->ReleaseStringUTFChars(env, controlsJson, json);
}

/* ---------- updateOverlayPixels (external pixel upload for GL overlay) ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_updateOverlayPixels(
    JNIEnv *env, jobject bridge, jlong handle, jobject pixelBuffer, jint width, jint height)
{
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;

    void *pixels = (*env)->GetDirectBufferAddress(env, pixelBuffer);
    if (!pixels || width <= 0 || height <= 0) return;

    static int overlay_log_count = 0;
    if (overlay_log_count < 5) {
        fprintf(stderr, "[player_bridge] updateOverlayPixels: %dx%d (frame %d)\n", width, height, overlay_log_count);
        overlay_log_count++;
    }

    int byte_count = width * height * 4; /* BGRA = 4 bytes per pixel */

    pthread_mutex_lock(&p->overlay_mutex);

    /* Reallocate staging buffer if size changed */
    if (p->overlay_staging_width != width || p->overlay_staging_height != height) {
        free(p->overlay_staging);
        p->overlay_staging = (unsigned char*)malloc(byte_count);
        if (!p->overlay_staging) {
            p->overlay_staging_width = 0;
            p->overlay_staging_height = 0;
            pthread_mutex_unlock(&p->overlay_mutex);
            return;
        }
        p->overlay_staging_width = width;
        p->overlay_staging_height = height;
    }

    /* Copy BGRA pixels to staging buffer */
    memcpy(p->overlay_staging, pixels, byte_count);
    p->overlay_dirty = 1;

    pthread_mutex_unlock(&p->overlay_mutex);

    /* Wake render thread to upload + composite */
    pthread_mutex_lock(&p->render_mutex);
    p->render_needed = 1;
    pthread_cond_signal(&p->render_cond);
    pthread_mutex_unlock(&p->render_mutex);
}

/* ---------- requestFocus ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_requestFocus(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (p->web_view) {
        /* Dispatch focus request to GTK thread */
        char *script = strdup("window.focus&&window.focus()");
        if (script) dispatch_js(p, script);
    }
}

/* ---------- setPaused ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setPaused(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jboolean paused)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    int flag = paused ? 1 : 0;
    g_libs.mpv_set_property(p->mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

/* ---------- seekTo ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekTo(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jlong positionMs)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    char pos_str[64];
    snprintf(pos_str, sizeof(pos_str), "%.3f", positionMs / 1000.0);
    const char *cmd[] = {"seek", pos_str, "absolute", NULL};
    g_libs.mpv_command(p->mpv, cmd);
}

/* ---------- seekBy ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekBy(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jlong offsetMs)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    char off_str[64];
    snprintf(off_str, sizeof(off_str), "%.3f", offsetMs / 1000.0);
    const char *cmd[] = {"seek", off_str, "relative", NULL};
    g_libs.mpv_command(p->mpv, cmd);
}

/* ---------- setSpeed ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSpeed(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jfloat speed)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    double spd = (double)speed;
    g_libs.mpv_set_property(p->mpv, "speed", MPV_FORMAT_DOUBLE, &spd);
}

/* ---------- adjustVolume ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_adjustVolume(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jfloat delta)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    double current_vol = 0.0;
    g_libs.mpv_get_property(p->mpv, "volume", MPV_FORMAT_DOUBLE, &current_vol);
    double new_vol = current_vol + (double)delta * 100.0;
    if (new_vol < 0.0) new_vol = 0.0;
    if (new_vol > 100.0) new_vol = 100.0;
    g_libs.mpv_set_property(p->mpv, "volume", MPV_FORMAT_DOUBLE, &new_vol);
}

/* ---------- setVolume ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setVolume(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jfloat level)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    double vol = (double)level * 100.0;
    if (vol < 0.0) vol = 0.0;
    if (vol > 100.0) vol = 100.0;
    g_libs.mpv_set_property(p->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

/* ---------- volume ---------- */
JNIEXPORT jfloat JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_volume(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return 0.0f;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return 0.0f;
    double vol = 0.0;
    g_libs.mpv_get_property(p->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    float result = (float)(vol / 100.0);
    if (result < 0.0f) result = 0.0f;
    if (result > 1.0f) result = 1.0f;
    return result;
}


/* ---------- setResizeMode ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setResizeMode(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jint mode)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;

    switch (mode) {
        case 0: /* Fit */
            g_libs.mpv_set_property_string(p->mpv, "keepaspect", "yes");
            {
                double panscan = 0.0;
                g_libs.mpv_set_property(p->mpv, "panscan", MPV_FORMAT_DOUBLE, &panscan);
                double zoom = 0.0;
                g_libs.mpv_set_property(p->mpv, "video-zoom", MPV_FORMAT_DOUBLE, &zoom);
            }
            break;
        case 1: /* Fill */ {
            double panscan = 1.0;
            g_libs.mpv_set_property(p->mpv, "panscan", MPV_FORMAT_DOUBLE, &panscan);
            double zoom = 0.0;
            g_libs.mpv_set_property(p->mpv, "video-zoom", MPV_FORMAT_DOUBLE, &zoom);
            g_libs.mpv_set_property_string(p->mpv, "keepaspect", "yes");
            break;
        }
        case 2: /* Zoom */ {
            double panscan = 1.0;
            g_libs.mpv_set_property(p->mpv, "panscan", MPV_FORMAT_DOUBLE, &panscan);
            double zoom = 1.0;
            g_libs.mpv_set_property(p->mpv, "video-zoom", MPV_FORMAT_DOUBLE, &zoom);
            g_libs.mpv_set_property_string(p->mpv, "keepaspect", "yes");
            break;
        }
        case 3: /* Stretch */
            g_libs.mpv_set_property_string(p->mpv, "keepaspect", "no");
            {
                double panscan = 0.0;
                g_libs.mpv_set_property(p->mpv, "panscan", MPV_FORMAT_DOUBLE, &panscan);
                double zoom = 0.0;
                g_libs.mpv_set_property(p->mpv, "video-zoom", MPV_FORMAT_DOUBLE, &zoom);
            }
            break;
        default:
            break;
    }
}

/* ---------- durationMs ---------- */
JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_durationMs(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return 0;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return 0;
    double dur = 0.0;
    int err = g_libs.mpv_get_property(p->mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
    if (err < 0 || dur < 0) return 0;
    return (jlong)(dur * 1000.0);
}

/* ---------- positionMs ---------- */
JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_positionMs(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return 0;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return 0;
    double pos = 0.0;
    int err = g_libs.mpv_get_property(p->mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    if (err < 0 || pos < 0) return 0;
    return (jlong)(pos * 1000.0);
}

/* ---------- bufferedPositionMs ---------- */
JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_bufferedPositionMs(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return 0;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return 0;

    /* mpv provides demuxer-cache-time which is relative to current position */
    double cache_time = 0.0;
    double position = 0.0;
    g_libs.mpv_get_property(p->mpv, "time-pos", MPV_FORMAT_DOUBLE, &position);
    int err = g_libs.mpv_get_property(p->mpv, "demuxer-cache-time", MPV_FORMAT_DOUBLE, &cache_time);
    if (err < 0) {
        /* Fallback: try demuxer-cache-duration (relative seconds) */
        double cache_dur = 0.0;
        err = g_libs.mpv_get_property(p->mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &cache_dur);
        if (err >= 0 && cache_dur > 0) {
            return (jlong)((position + cache_dur) * 1000.0);
        }
        return (jlong)(position * 1000.0);
    }
    return (jlong)(cache_time * 1000.0);
}

/* ---------- isLoading ---------- */
JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isLoading(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return JNI_FALSE;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    return p->is_loading ? JNI_TRUE : JNI_FALSE;
}

/* ---------- isEnded ---------- */
JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isEnded(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return JNI_FALSE;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    return p->is_ended ? JNI_TRUE : JNI_FALSE;
}

/* ---------- isPaused ---------- */
JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isPaused(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return JNI_TRUE;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return JNI_TRUE;
    int paused = 1;
    g_libs.mpv_get_property(p->mpv, "pause", MPV_FORMAT_FLAG, &paused);
    return paused ? JNI_TRUE : JNI_FALSE;
}

/* ---------- speed ---------- */
JNIEXPORT jfloat JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_speed(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return 1.0f;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return 1.0f;
    double spd = 1.0;
    g_libs.mpv_get_property(p->mpv, "speed", MPV_FORMAT_DOUBLE, &spd);
    return (jfloat)spd;
}

/* ---------- audioTracksJson ---------- */
JNIEXPORT jstring JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_audioTracksJson(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)bridge;
    if (handle == 0) return (*env)->NewStringUTF(env, "[]");
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;

    char buf[8192];
    build_tracks_json(p, "audio", 0, buf, sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

/* ---------- subtitleTracksJson ---------- */
JNIEXPORT jstring JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_subtitleTracksJson(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)bridge;
    if (handle == 0) return (*env)->NewStringUTF(env, "[]");
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;

    char buf[8192];
    build_tracks_json(p, "sub", 1, buf, sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

/* ---------- selectAudioTrack ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectAudioTrack(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jint trackId)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", (int)trackId);
    g_libs.mpv_set_property_string(p->mpv, "aid", id_str);
}

/* ---------- selectSubtitleTrack ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectSubtitleTrack(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jint trackId)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    if (trackId < 0) {
        g_libs.mpv_set_property_string(p->mpv, "sid", "no");
    } else {
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%d", (int)trackId);
        g_libs.mpv_set_property_string(p->mpv, "sid", id_str);
    }
}

/* ---------- addSubtitleUrl ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_addSubtitleUrl(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jstring url)
{
    (void)bridge;
    if (handle == 0 || !url) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;

    const char *url_chars = (*env)->GetStringUTFChars(env, url, NULL);
    if (!url_chars) return;

    /* Get current sub track count before adding */
    int64_t count_before = 0;
    mpv_node node;
    memset(&node, 0, sizeof(node));
    if (g_libs.mpv_get_property(p->mpv, "track-list", MPV_FORMAT_NODE, &node) >= 0) {
        if (node.format == MPV_FORMAT_NODE_ARRAY && node.u.list) {
            for (int i = 0; i < node.u.list->num; i++) {
                if (node.u.list->values[i].format == MPV_FORMAT_NODE_MAP && node.u.list->values[i].u.list) {
                    mpv_node_list *track = node.u.list->values[i].u.list;
                    for (int k = 0; k < track->num; k++) {
                        if (track->keys[k] && strcmp(track->keys[k], "type") == 0 &&
                            track->values[k].format == MPV_FORMAT_STRING &&
                            strcmp(track->values[k].u.string, "sub") == 0) {
                            count_before++;
                            break;
                        }
                    }
                }
            }
        }
        g_libs.mpv_free_node_contents(&node);
    }

    const char *cmd[] = {"sub-add", url_chars, NULL};
    g_libs.mpv_command(p->mpv, cmd);

    /* Track the newly added subtitle (assumes it gets the next ID) */
    track_external_sub(p, (int)(count_before + 1));

    (*env)->ReleaseStringUTFChars(env, url, url_chars);
}

/* ---------- clearExternalSubtitles ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitles(
    JNIEnv *env,
    jobject bridge,
    jlong handle)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;

    /* Remove all tracked external subtitle tracks */
    for (int i = p->external_sub_count - 1; i >= 0; i--) {
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%d", p->external_sub_ids[i]);
        const char *cmd[] = {"sub-remove", id_str, NULL};
        g_libs.mpv_command(p->mpv, cmd);
    }
    p->external_sub_count = 0;
}

/* ---------- clearExternalSubtitlesAndSelect ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitlesAndSelect(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jint trackId)
{
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;

    /* Remove external subtitles */
    for (int i = p->external_sub_count - 1; i >= 0; i--) {
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%d", p->external_sub_ids[i]);
        const char *cmd[] = {"sub-remove", id_str, NULL};
        g_libs.mpv_command(p->mpv, cmd);
    }
    p->external_sub_count = 0;

    /* Select the specified track */
    if (trackId < 0) {
        g_libs.mpv_set_property_string(p->mpv, "sid", "no");
    } else {
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%d", (int)trackId);
        g_libs.mpv_set_property_string(p->mpv, "sid", id_str);
    }

    (void)env;
}

/* ---------- setSubtitleDelayMs ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSubtitleDelayMs(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jint delayMs)
{
    (void)env;
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;
    double delay_sec = (double)delayMs / 1000.0;
    g_libs.mpv_set_property(p->mpv, "sub-delay", MPV_FORMAT_DOUBLE, &delay_sec);
}

/* ---------- applySubtitleStyle ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applySubtitleStyle(
    JNIEnv *env,
    jobject bridge,
    jlong handle,
    jstring textColor,
    jstring backgroundColor,
    jstring outlineColor,
    jfloat outlineSize,
    jboolean bold,
    jfloat fontSize,
    jint subPos)
{
    (void)bridge;
    if (handle == 0) return;
    PlayerInstance *p = (PlayerInstance*)(intptr_t)handle;
    if (!p->mpv) return;

    if (textColor) {
        const char *val = (*env)->GetStringUTFChars(env, textColor, NULL);
        if (val) {
            g_libs.mpv_set_property_string(p->mpv, "sub-color", val);
            (*env)->ReleaseStringUTFChars(env, textColor, val);
        }
    }

    if (backgroundColor) {
        const char *val = (*env)->GetStringUTFChars(env, backgroundColor, NULL);
        if (val) {
            g_libs.mpv_set_property_string(p->mpv, "sub-back-color", val);
            (*env)->ReleaseStringUTFChars(env, backgroundColor, val);
        }
    }

    if (outlineColor) {
        const char *val = (*env)->GetStringUTFChars(env, outlineColor, NULL);
        if (val) {
            g_libs.mpv_set_property_string(p->mpv, "sub-border-color", val);
            (*env)->ReleaseStringUTFChars(env, outlineColor, val);
        }
    }

    double border_size = (double)outlineSize;
    g_libs.mpv_set_property(p->mpv, "sub-border-size", MPV_FORMAT_DOUBLE, &border_size);

    g_libs.mpv_set_property_string(p->mpv, "sub-bold", bold ? "yes" : "no");

    double font_size = (double)fontSize;
    g_libs.mpv_set_property(p->mpv, "sub-font-size", MPV_FORMAT_DOUBLE, &font_size);

    int64_t sub_position = (int64_t)subPos;
    g_libs.mpv_set_property(p->mpv, "sub-pos", MPV_FORMAT_INT64, &sub_position);
}


/* ---------- warmupWebView2 (stub - Windows only) ---------- */
JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_warmupWebView2(
    JNIEnv *env,
    jobject bridge,
    jstring controlsPageUrl)
{
    (void)env;
    (void)bridge;
    (void)controlsPageUrl;
    /* No-op on Linux - WebView2 is Windows-only */
    return JNI_FALSE;
}

/* ---------- shutdownWebView2Warmup (stub - Windows only) ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_shutdownWebView2Warmup(
    JNIEnv *env,
    jobject bridge)
{
    (void)env;
    (void)bridge;
    /* No-op on Linux */
}

/* ---------- applyWindowChrome (stub - Windows only) ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applyWindowChrome(
    JNIEnv *env,
    jobject bridge,
    jlong windowHwnd,
    jboolean darkMode,
    jint captionColorRgb,
    jint borderColorRgb,
    jint textColorRgb)
{
    (void)env;
    (void)bridge;
    (void)windowHwnd;
    (void)darkMode;
    (void)captionColorRgb;
    (void)borderColorRgb;
    (void)textColorRgb;
    /* No-op on Linux - window chrome customization is Windows-specific */
}

/* ---------- setWindowBorderlessFullscreen (stub - Windows only) ---------- */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setWindowBorderlessFullscreen(
    JNIEnv *env,
    jobject bridge,
    jlong windowHwnd,
    jboolean fullscreen,
    jint x,
    jint y,
    jint width,
    jint height)
{
    (void)env;
    (void)bridge;
    (void)windowHwnd;
    (void)fullscreen;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    /* No-op on Linux - borderless fullscreen is handled differently on Linux */
}

/* ---------- loadLibraryGlobal ---------- */
/* Loads a shared library with RTLD_GLOBAL so its symbols override existing ones
 * for subsequent dlopen calls. */
JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_loadLibraryGlobal(
    JNIEnv *env,
    jobject bridge,
    jstring path)
{
    (void)bridge;
    const char *path_chars = (*env)->GetStringUTFChars(env, path, NULL);
    if (!path_chars) return;

    void *handle = dlopen(path_chars, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "[player_bridge] loadLibraryGlobal failed: %s\n", dlerror());
    } else {
        fprintf(stderr, "[player_bridge] loadLibraryGlobal OK: %s\n", path_chars);
    }

    (*env)->ReleaseStringUTFChars(env, path, path_chars);
}

/* ---------- initGtkEarly ---------- */
/* Must be called BEFORE any AWT/Swing/Compose/Skia initialization.
 * Initializes GTK so that GDK types are registered properly BEFORE Skiko
 * loads libgdk-3 and partially registers GdkDisplayManager. */
JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_initGtkEarly(
    JNIEnv *env,
    jclass cls)
{
    (void)env;
    (void)cls;
    
    void *gtk_lib = dlopen("libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!gtk_lib) {
        fprintf(stderr, "[player_bridge] initGtkEarly: failed to load libgtk-3.so.0: %s\n", dlerror());
        return JNI_FALSE;
    }
    
    typedef int (*fn_gtk_init_check_t)(int*, char***);
    fn_gtk_init_check_t init_fn = (fn_gtk_init_check_t)dlsym(gtk_lib, "gtk_init_check");
    if (!init_fn) {
        fprintf(stderr, "[player_bridge] initGtkEarly: gtk_init_check symbol not found\n");
        return JNI_FALSE;
    }
    
    int argc = 0;
    if (!init_fn(&argc, NULL)) {
        fprintf(stderr, "[player_bridge] initGtkEarly: gtk_init_check returned FALSE\n");
        return JNI_FALSE;
    }
    
    fprintf(stderr, "[player_bridge] initGtkEarly: GTK initialized successfully (before AWT/Skia)\n");
    return JNI_TRUE;
}
