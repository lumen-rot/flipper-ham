#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "build_config.h"
#include "profile_store.h"
#include <ham_icons.h>

#define HAM_DIR "/ext/apps_data/ham"
#define HAM_TEXT HAM_DIR "/text.txt"
#define TEXT_CAPACITY 8192

static const char sample_text[] =
    "I am trying a few ideas and taking a moment to think about them. "
    "The first approach seems useful, but I would like to explore another example. "
    "Could you explain that last point in a little more detail? "
    "Let me read through these notes before I continue. "
    "That gives me something to work with. I will come back to the earlier question. "
    "Here is a shorter version of the thought I was working on. ";
static const char default_profile[] =
    "# HAM profile. Changes take effect when the app opens.\n"
    "activity=50\nradius=600\nduration_seconds=600\n"
    "wpm_min=100\nwpm_max=130\nclick_percent=25\nseed=0\n";

typedef struct {
    HamEngine engine;
    char text[TEXT_CAPACITY];
    FuriMutex* mutex;
    FuriMessageQueue* input;
    FuriEventFlag* control;
    bool connected;
    bool usb_failed;
    bool duration_selected;
#if HAM_ENABLE_PREVIEW
    bool capture;
    bool captured;
    unsigned capture_pose;
    uint8_t frame[1024];
#endif
} HamApp;

static size_t read_file(Storage* storage, const char* path, char* dst, size_t capacity) {
    File* file = storage_file_alloc(storage);
    size_t size = 0;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING))
        size = storage_file_read(file, dst, capacity - 1);
    dst[size] = 0;
    storage_file_close(file);
    storage_file_free(file);
    return size;
}
static void create_file(Storage* storage, const char* path, const char* content) {
    FileInfo info;
    if(storage_common_stat(storage, path, &info) == FSE_OK) return;
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW))
        storage_file_write(file, content, strlen(content));
    storage_file_close(file);
    storage_file_free(file);
}
static unsigned limit(unsigned n, unsigned low, unsigned high) {
    return n < low ? low : n > high ? high : n;
}
static HamProfile load_profile(HamApp* app) {
    HamProfile profile = ham_defaults();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, HAM_DIR);
    ham_profile_recover(storage);
    const char* profile_path = ham_profile_source(storage);
    if(!strcmp(profile_path, HAM_PROFILE)) create_file(storage, HAM_PROFILE, default_profile);
    create_file(storage, HAM_TEXT, sample_text);
    char config[1024];
    read_file(storage, profile_path, config, sizeof(config));
    char* line = config;
    while(*line) {
        char* end = strchr(line, '\n');
        if(end) *end = 0;
        char key[32];
        unsigned value;
        if(sscanf(line, "%31[^=]=%u", key, &value) != 2) {
            /* Comments and blank lines are ignored. */
        } else if(!strcmp(key, "activity")) profile.activity = limit(value, 0, 100);
        else if(!strcmp(key, "radius")) profile.radius = limit(value, 5, 1200);
        else if(!strcmp(key, "duration_seconds"))
            profile.duration_seconds = value ? limit(value, 10, 7200) : 0;
        else if(!strcmp(key, "wpm_min")) profile.wpm_min = limit(value, 15, 180);
        else if(!strcmp(key, "wpm_max")) profile.wpm_max = limit(value, 15, 180);
        else if(!strcmp(key, "click_percent")) profile.click_percent = limit(value, 0, 100);
        else if(!strcmp(key, "seed")) profile.seed = value;
        if(!end) break;
        line = end + 1;
    }
    if(profile.wpm_max < profile.wpm_min) profile.wpm_max = profile.wpm_min;
    read_file(storage, HAM_TEXT, app->text, sizeof(app->text));
    furi_record_close(RECORD_STORAGE);
    /* A flat printable stream: never execute tabs, returns, shortcuts or control bytes. */
    size_t len = strlen(app->text), out = 0;
    bool space = true;
    for(size_t i = 0; i < len && out < sizeof(app->text) - 2; ++i) {
        unsigned char c = app->text[i];
        if(c < 32 || c > 126) c = ' ';
        if(c == ' ' && space) continue;
        app->text[out++] = c;
        space = c == ' ';
    }
    if(!out) {
        strcpy(app->text, sample_text);
    } else {
        if(!space) app->text[out++] = ' ';
        app->text[out] = 0;
    }
    return profile;
}
static bool save_profile(const HamProfile* p) {
    char config[512];
    snprintf(config, sizeof(config),
             "# HAM profile. Radius is in relative HID counts.\n"
             "activity=%u\nradius=%u\nduration_seconds=%u\n"
             "wpm_min=%u\nwpm_max=%u\nclick_percent=%u\nseed=%lu\n",
             p->activity, p->radius, p->duration_seconds, p->wpm_min, p->wpm_max,
             p->click_percent, (unsigned long)p->seed);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool saved = ham_profile_store(storage, config);
    furi_record_close(RECORD_STORAGE);
    return saved;
}
static void release_inputs(void) {
    furi_hal_hid_kb_release_all();
    furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT | HID_MOUSE_BTN_RIGHT | HID_MOUSE_BTN_WHEEL);
}
static bool dispatch(HamEvent event) {
    uint16_t key = event.character == '\b' ? HID_KEYBOARD_DELETE : HID_ASCII_TO_KEY(event.character);
    switch(event.kind) {
    case HamKeyDown: return furi_hal_hid_kb_press(key);
    case HamKeyUp: return furi_hal_hid_kb_release(key);
    case HamMouseMove: return furi_hal_hid_mouse_move(event.dx, event.dy);
    case HamClickDown: return furi_hal_hid_mouse_press(HID_MOUSE_BTN_LEFT);
    case HamClickUp: return furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
    default: return true;
    }
}
static void input_callback(InputEvent* event, void* context) {
    HamApp* app = context;
    /* Back bypasses the queue so a repeat flood cannot delay or drop an exit.
       Never block the GUI callback waiting for the app to consume input. */
    if(event->key == InputKeyBack) {
        if(event->type == InputTypePress || event->type == InputTypeShort)
            furi_event_flag_set(app->control, 1);
    } else if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        furi_message_queue_put(app->input, event, 0);
    }
}
static void draw_callback(Canvas* canvas, void* context) {
    HamApp* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const HamEngine* e = &app->engine;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    unsigned pose = e->running ? (furi_get_tick() / 220) % 2 : 0;
#if HAM_ENABLE_PREVIEW
    if(app->capture && e->running) pose = app->capture_pose;
#endif
    canvas_draw_icon(canvas, 3 + pose, 1 + (pose ? 0 : 1), &I_ham);
    canvas_draw_str(canvas, 18, 11, "HAM");
    canvas_set_font(canvas, FontSecondary);
    const char* status = app->usb_failed ? "USB busy" : !app->connected ? "No USB" : ham_episode_name(e);
    canvas_draw_str_aligned(canvas, 125, 11, AlignRight, AlignBottom, status);
    char number[12];
    snprintf(number, sizeof(number), "%u", e->profile.activity);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 60, 35, AlignRight, AlignBottom, number);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 64, 33, "% activity");
    if(!app->duration_selected) canvas_draw_str(canvas, 3, 29, ">");
    canvas_draw_frame(canvas, 4, 38, 120, 4);
    if(e->profile.activity) canvas_draw_box(canvas, 5, 39, e->profile.activity * 118 / 100, 2);
    if(app->duration_selected) canvas_draw_str(canvas, 3, 53, ">");
    canvas_draw_str(canvas, 12, 53, "Time");
    char duration[12];
    unsigned seconds = e->profile.duration_seconds;
    if(seconds) {
        if(seconds % 3600 == 0) snprintf(duration, sizeof(duration), "%uh", seconds / 3600);
        else if(seconds % 60 == 0) snprintf(duration, sizeof(duration), "%um", seconds / 60);
        else snprintf(duration, sizeof(duration), "%us", seconds);
        canvas_draw_str(canvas, 40, 53, duration);
    } else {
        /* Draw infinity explicitly; the device font does not contain Unicode infinity. */
        static const int8_t points[][2] = {
            {0,3},{1,1},{3,1},{5,3},{7,5},{9,5},{10,3},{9,1},{7,1},{5,3},{3,5},{1,5},{0,3}};
        for(unsigned i = 1; i < COUNT_OF(points); ++i)
            canvas_draw_line(canvas, 41 + points[i-1][0], 45 + points[i-1][1],
                             41 + points[i][0], 45 + points[i][1]);
    }
    uint64_t elapsed = e->elapsed / 1000;
    uint64_t clock_seconds = seconds ? (elapsed < seconds ? seconds - elapsed : 0) : elapsed;
    char clock[28];
    if(clock_seconds >= 3600)
        snprintf(clock, sizeof(clock), "%lu:%02u:%02u", (unsigned long)(clock_seconds / 3600),
                 (unsigned)(clock_seconds / 60 % 60), (unsigned)(clock_seconds % 60));
    else snprintf(clock, sizeof(clock), "%u:%02u %s", (unsigned)(clock_seconds / 60),
                  (unsigned)(clock_seconds % 60), seconds ? "left" : "run");
    canvas_draw_str_aligned(canvas, 124, 53, AlignRight, AlignBottom, clock);
    canvas_draw_str(canvas, 4, 64, e->complete ? "OK  New session" : e->running ? "OK  Pause" : "OK  Start");
    canvas_draw_str_aligned(canvas, 124, 64, AlignRight, AlignBottom, "Back  Exit");
#if HAM_ENABLE_PREVIEW
    if(app->capture && canvas_get_buffer_size(canvas) == sizeof(app->frame)) {
        memcpy(app->frame, canvas_get_buffer(canvas), sizeof(app->frame));
        app->captured = true;
    }
#endif
    furi_mutex_release(app->mutex);
}

int32_t ham_app(void* context) {
    HamApp* app = calloc(1, sizeof(HamApp));
    bool is_preview = false;
#if HAM_ENABLE_PREVIEW
    /* Diagnostic rendering mode: captures the actual device canvas without HID output. */
    app->capture = context && !strncmp((const char*)context, "preview", 7);
    is_preview = app->capture;
#else
    UNUSED(context);
#endif
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->control = furi_event_flag_alloc();
    HamProfile profile = load_profile(app);
    ham_init(&app->engine, profile, app->text, profile.seed ? profile.seed : furi_hal_random_get());
#if HAM_ENABLE_PREVIEW
    if(app->capture && strstr((const char*)context, "running")) {
        app->engine.running = true; /* Render-only diagnostics: engine is never ticked or dispatched. */
        app->capture_pose = strstr((const char*)context, "-b") ? 1 : 0;
    }
    if(app->capture && strstr((const char*)context, "infinite")) {
        app->engine.profile.duration_seconds = 0;
        app->duration_selected = true;
    }
#endif
    FuriHalUsbInterface* previous_usb = furi_hal_usb_get_config();
    app->usb_failed = !is_preview && !furi_hal_usb_set_config(&usb_hid, NULL);
    Gui* gui = furi_record_open(RECORD_GUI);
    ViewPort* view = view_port_alloc();
    view_port_draw_callback_set(view, draw_callback, app);
    view_port_input_callback_set(view, input_callback, app);
    gui_add_view_port(gui, view, GuiLayerFullscreen);
    bool exit = false;
    uint32_t redraw = 0;
#if HAM_ENABLE_PREVIEW
    uint32_t opened = furi_get_tick();
#endif
    while(!exit) {
        InputEvent input;
        FuriStatus received = furi_message_queue_get(app->input, &input, 10);
        exit = (furi_event_flag_get(app->control) & 1) != 0;
        uint32_t now = furi_get_tick();
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool connected = is_preview || (!app->usb_failed && furi_hal_hid_is_connected());
        if(!connected && app->engine.running) {
            release_inputs();
            ham_pause(&app->engine);
        }
        app->connected = connected;
#if HAM_ENABLE_PREVIEW
        if(is_preview) {
            exit = exit || now - opened > 800;
        } else
#endif
        if(!exit && received == FuriStatusOk) {
            if(input.key == InputKeyBack) exit = true;
            else if(input.key == InputKeyUp) app->duration_selected = false;
            else if(input.key == InputKeyDown) app->duration_selected = true;
            else if(input.key == InputKeyLeft || input.key == InputKeyRight) {
                if(app->duration_selected) {
                    app->engine.profile.duration_seconds = ham_duration_step(
                        app->engine.profile.duration_seconds, input.key == InputKeyRight ? 1 : -1);
                } else {
                    int a = (int)app->engine.profile.activity + (input.key == InputKeyRight ? 5 : -5);
                    ham_activity(&app->engine, a < 0 ? 0 : a > 100 ? 100 : a, now);
                    if(!app->engine.running && !app->usb_failed) release_inputs();
                }
            } else if(input.key == InputKeyOk && input.type == InputTypeShort) {
                if(app->engine.running) {
                    release_inputs();
                    ham_pause(&app->engine);
                } else if(connected) {
                    if(app->engine.complete) {
                        HamProfile current = app->engine.profile;
                        ham_init(&app->engine, current, app->text,
                                 current.seed ? current.seed : furi_hal_random_get());
                    }
                    ham_start(&app->engine, now);
                }
            }
        }
        bool was_running = app->engine.running;
        if(!exit && !is_preview) {
            HamEvent event = ham_tick(&app->engine, now);
            if(!dispatch(event)) {
                release_inputs();
                ham_pause(&app->engine);
            }
            if(was_running && !app->engine.running) release_inputs();
        }
        furi_mutex_release(app->mutex);
        if(now - redraw >= 100 || received == FuriStatusOk) {
            view_port_update(view);
            redraw = now;
        }
    }
    if(!app->usb_failed && !is_preview) release_inputs();
    view_port_enabled_set(view, false);
    gui_remove_view_port(gui, view);
    view_port_free(view);
    furi_record_close(RECORD_GUI);
    if(!app->usb_failed && !is_preview) furi_hal_usb_set_config(previous_usb, NULL);
    if(!is_preview && !save_profile(&app->engine.profile)) {
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        dialog_message_show_storage_error(dialogs, "Could not save\nHAM settings.");
        furi_record_close(RECORD_DIALOGS);
    }
#if HAM_ENABLE_PREVIEW
    if(app->capture && app->captured) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        File* file = storage_file_alloc(storage);
        if(storage_file_open(file, HAM_DIR "/preview.bin", FSAM_WRITE, FSOM_CREATE_ALWAYS))
            storage_file_write(file, app->frame, sizeof(app->frame));
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
    }
#endif
    furi_message_queue_free(app->input);
    furi_event_flag_free(app->control);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
