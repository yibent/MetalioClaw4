#include "lua_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lauxlib.h"
#include "lua_runtime_internal.h"
#include "lvgl.h"

#define UI_CONTEXT_METATABLE "metalio.lua.ui.context"
#define UI_MAX_OBJECTS 64
#define UI_EVENT_QUEUE_LENGTH 24
#define UI_EVENT_ID_LENGTH 32
#define LUA_UI_SAVED_CLICKABLE_MAX 32

typedef enum {
    UI_OBJECT_NONE = 0,
    UI_OBJECT_SCREEN,
    UI_OBJECT_RECT,
    UI_OBJECT_LABEL,
    UI_OBJECT_BUTTON,
    UI_OBJECT_CIRCLE,
    UI_OBJECT_LINE,
    UI_OBJECT_ARC,
    UI_OBJECT_IMAGE,
} ui_object_type_t;

typedef struct ui_context ui_context_t;

typedef struct {
    int id;
    ui_object_type_t type;
    lv_obj_t* object;
    ui_context_t* context;
    char event_id[UI_EVENT_ID_LENGTH];
    void* owned_data;
} ui_object_t;

typedef struct {
    int object_id;
    char event_id[UI_EVENT_ID_LENGTH];
    char type[16];
    int16_t x;
    int16_t y;
    int16_t dx;
    int16_t dy;
    uint32_t time_ms;
} ui_event_t;

struct ui_context {
    QueueHandle_t events;
    lv_obj_t* screen;
    lv_obj_t* previous_screen;
    ui_object_t objects[UI_MAX_OBJECTS];
    int next_id;
    bool closed;
    bool touch_active;
    int16_t touch_x;
    int16_t touch_y;
};

static const char kContextRegistryKey;
static ui_context_t* s_screen_owner;
static bool s_exclusive_input;
static lv_obj_t* s_saved_clickable[LUA_UI_SAVED_CLICKABLE_MAX];
static uint8_t s_saved_clickable_count;
static lv_fs_drv_t s_native_fs_driver;
static bool s_native_fs_registered;
static lua_runtime_ui_lock_callback_t s_ui_lock;
static lua_runtime_ui_unlock_callback_t s_ui_unlock;
static void* s_ui_backend_context;

static bool ui_lock(void) {
    return s_ui_lock && s_ui_lock(-1, s_ui_backend_context);
}

static void ui_unlock(void) { s_ui_unlock(s_ui_backend_context); }

esp_err_t lua_runtime_set_ui_backend(lua_runtime_ui_lock_callback_t lock,
                                     lua_runtime_ui_unlock_callback_t unlock, void* user_ctx) {
    if ((lock == NULL) != (unlock == NULL))
        return ESP_ERR_INVALID_ARG;
    s_ui_lock = lock;
    s_ui_unlock = unlock;
    s_ui_backend_context = lock ? user_ctx : NULL;
    return ESP_OK;
}

static void free_owned_data(ui_object_t* entry) {
    free(entry->owned_data);
    entry->owned_data = NULL;
}

static void* native_fs_open(lv_fs_drv_t* driver, const char* path, lv_fs_mode_t mode) {
    (void)driver;
    if (mode != LV_FS_MODE_RD || !path || path[0] != '/')
        return NULL;
    return fopen(path, "rb");
}

static lv_fs_res_t native_fs_close(lv_fs_drv_t* driver, void* file) {
    (void)driver;
    return fclose(file) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t native_fs_read(lv_fs_drv_t* driver, void* file, void* buffer, uint32_t requested,
                                  uint32_t* read_count) {
    (void)driver;
    size_t count = fread(buffer, 1, requested, file);
    if (read_count)
        *read_count = (uint32_t)count;
    return ferror(file) ? LV_FS_RES_FS_ERR : LV_FS_RES_OK;
}

static lv_fs_res_t native_fs_seek(lv_fs_drv_t* driver, void* file, uint32_t position,
                                  lv_fs_whence_t whence) {
    (void)driver;
    int origin = whence == LV_FS_SEEK_CUR   ? SEEK_CUR
                 : whence == LV_FS_SEEK_END ? SEEK_END
                                            : SEEK_SET;
    return fseek(file, (long)position, origin) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t native_fs_tell(lv_fs_drv_t* driver, void* file, uint32_t* position) {
    (void)driver;
    long value = ftell(file);
    if (value < 0)
        return LV_FS_RES_FS_ERR;
    *position = (uint32_t)value;
    return LV_FS_RES_OK;
}

static void register_native_fs(void) {
    if (s_native_fs_registered)
        return;
    lv_fs_drv_init(&s_native_fs_driver);
    s_native_fs_driver.letter = 'L';
    s_native_fs_driver.open_cb = native_fs_open;
    s_native_fs_driver.close_cb = native_fs_close;
    s_native_fs_driver.read_cb = native_fs_read;
    s_native_fs_driver.seek_cb = native_fs_seek;
    s_native_fs_driver.tell_cb = native_fs_tell;
    lv_fs_drv_register(&s_native_fs_driver);
    s_native_fs_registered = true;
}

static uint32_t read_color(lua_State* state, int table_index, const char* field,
                           uint32_t default_value) {
    lua_getfield(state, table_index, field);
    uint32_t value = lua_isinteger(state, -1) ? (uint32_t)lua_tointeger(state, -1) : default_value;
    lua_pop(state, 1);
    return value;
}

static int read_integer(lua_State* state, int table_index, const char* field, int default_value) {
    lua_getfield(state, table_index, field);
    int value = lua_isinteger(state, -1) ? (int)lua_tointeger(state, -1) : default_value;
    lua_pop(state, 1);
    return value;
}

static const char* read_string(lua_State* state, int table_index, const char* field,
                               const char* default_value) {
    lua_getfield(state, table_index, field);
    const char* value = lua_isstring(state, -1) ? lua_tostring(state, -1) : default_value;
    lua_pop(state, 1);
    return value;
}

static ui_context_t* get_context(lua_State* state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kContextRegistryKey);
    ui_context_t* context = lua_touserdata(state, -1);
    lua_pop(state, 1);
    if (context)
        return context;

    context = lua_newuserdatauv(state, sizeof(*context), 0);
    memset(context, 0, sizeof(*context));
    context->events = xQueueCreate(UI_EVENT_QUEUE_LENGTH, sizeof(ui_event_t));
    context->next_id = 1;
    if (!context->events)
        luaL_error(state, "ui event queue allocation failed");
    luaL_getmetatable(state, UI_CONTEXT_METATABLE);
    lua_setmetatable(state, -2);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kContextRegistryKey);
    return context;
}

static ui_object_t* find_object(ui_context_t* context, int id) {
    for (size_t i = 0; i < UI_MAX_OBJECTS; ++i) {
        if (context->objects[i].id == id && context->objects[i].object)
            return &context->objects[i];
    }
    return NULL;
}

static ui_object_t* add_object(ui_context_t* context, lv_obj_t* object, ui_object_type_t type,
                               const char* event_id) {
    for (size_t i = 0; i < UI_MAX_OBJECTS; ++i) {
        if (!context->objects[i].object) {
            ui_object_t* entry = &context->objects[i];
            memset(entry, 0, sizeof(*entry));
            entry->id = context->next_id++;
            entry->type = type;
            entry->object = object;
            entry->context = context;
            if (event_id)
                strlcpy(entry->event_id, event_id, sizeof(entry->event_id));
            return entry;
        }
    }
    return NULL;
}

static const char* event_type_name(lv_event_code_t code) {
    switch (code) {
        case LV_EVENT_PRESSED:
            return "pressed";
        case LV_EVENT_PRESSING:
            return "moved";
        case LV_EVENT_PRESS_LOST:
            return "lost";
        case LV_EVENT_RELEASED:
            return "released";
        case LV_EVENT_CLICKED:
            return "clicked";
        case LV_EVENT_VALUE_CHANGED:
            return "changed";
        default:
            return "event";
    }
}

static void object_event_callback(lv_event_t* event) {
    ui_object_t* entry = lv_event_get_user_data(event);
    if (!entry || !entry->context || !entry->context->events)
        return;
    lv_event_code_t code = lv_event_get_code(event);
    ui_event_t queued = {.object_id = entry->id};
    strlcpy(queued.event_id, entry->event_id, sizeof(queued.event_id));
    strlcpy(queued.type, event_type_name(lv_event_get_code(event)), sizeof(queued.type));
    lv_indev_t* indev = lv_event_get_indev(event);
    if (indev) {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        queued.x = point.x;
        queued.y = point.y;
        queued.dx = entry->context->touch_active ? point.x - entry->context->touch_x : 0;
        queued.dy = entry->context->touch_active ? point.y - entry->context->touch_y : 0;
        queued.time_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (code == LV_EVENT_PRESSING && queued.dx == 0 && queued.dy == 0)
            return;
        entry->context->touch_x = point.x;
        entry->context->touch_y = point.y;
        entry->context->touch_active = code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST;
    }
    if (xQueueSend(entry->context->events, &queued, 0) != pdTRUE && code != LV_EVENT_PRESSING) {
        ui_event_t discarded;
        xQueueReceive(entry->context->events, &discarded, 0);
        xQueueSend(entry->context->events, &queued, 0);
    }
}

static void add_touch_events(lv_obj_t* object, ui_object_t* entry) {
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_PRESSED, entry);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_PRESSING, entry);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_PRESS_LOST, entry);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_RELEASED, entry);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_CLICKED, entry);
}

static void apply_pointer_policy(lv_obj_t* object, ui_object_t* entry, const char* event_id) {
    if (event_id && event_id[0] != '\0') {
        add_touch_events(object, entry);
        return;
    }
    /* Decorative widgets must not eat hits; otherwise a dino/cactus/label
     * swallows the tap and ui.poll_event never sees it. */
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

static void reset_pointer_devices(void) {
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_wait_release(indev);
            lv_indev_reset(indev, NULL);
        }
        indev = lv_indev_get_next(indev);
    }
}

static void steal_clickable_recursive(lv_obj_t* obj) {
    if (!obj)
        return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        s_saved_clickable_count < LUA_UI_SAVED_CLICKABLE_MAX) {
        s_saved_clickable[s_saved_clickable_count++] = obj;
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i)
        steal_clickable_recursive(lv_obj_get_child(obj, i));
}

static void begin_exclusive_input(void) {
    /* Do not hide lv_layer_top() itself: it has no parent, and
     * lv_obj_remove_flag(HIDDEN) always dirties the parent. */
    if (!s_exclusive_input) {
        s_saved_clickable_count = 0;
        lv_obj_t* top = lv_layer_top();
        if (top)
            steal_clickable_recursive(top);
        s_exclusive_input = true;
    }
    reset_pointer_devices();
}

static void end_exclusive_input(void) {
    if (!s_exclusive_input)
        return;
    for (uint8_t i = 0; i < s_saved_clickable_count; ++i) {
        lv_obj_t* obj = s_saved_clickable[i];
        if (obj && lv_obj_is_valid(obj))
            lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        s_saved_clickable[i] = NULL;
    }
    s_saved_clickable_count = 0;
    reset_pointer_devices();
    s_exclusive_input = false;
}

static void apply_geometry(lv_obj_t* object, int x, int y, int width, int height) {
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

/* CubeMax simulator accepts ui.rect(parent, { ... }) as well as ui.rect({ parent = ... }). */
static int widget_table_index(lua_State* state) {
    if (lua_istable(state, 1) && (lua_gettop(state) < 2 || lua_isnoneornil(state, 2))) {
        return 1;
    }
    if (lua_isinteger(state, 1) && lua_istable(state, 2)) {
        lua_getfield(state, 2, "parent");
        const int missing = lua_isnoneornil(state, -1);
        lua_pop(state, 1);
        if (missing) {
            lua_pushvalue(state, 1);
            lua_setfield(state, 2, "parent");
        }
        return 2;
    }
    return luaL_error(state, "ui widget expects a table, or (parent, table)");
}

static lv_obj_t* read_parent(lua_State* state, ui_context_t* context, int table_index) {
    lua_getfield(state, table_index, "parent");
    int parent_id = lua_isinteger(state, -1) ? (int)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);
    if (parent_id) {
        ui_object_t* parent = find_object(context, parent_id);
        if (!parent)
            luaL_error(state, "invalid parent object");
        return parent->object;
    }
    if (!context->screen)
        luaL_error(state, "ui.screen() must be called first");
    return context->screen;
}

static int l_screen(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    ui_context_t* context = get_context(state);
    uint32_t background = read_color(state, 1, "background", 0x101418);
    if (context->screen)
        return luaL_error(state, "a Lua VM can own only one screen");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    if (s_screen_owner && s_screen_owner != context) {
        ui_unlock();
        return luaL_error(state, "the display is already owned by another Lua job");
    }
    context->screen = lv_obj_create(NULL);
    if (!context->screen) {
        ui_unlock();
        return luaL_error(state, "screen allocation failed");
    }
    lv_obj_remove_flag(context->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(context->screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(context->screen, lv_color_hex(background), 0);
    ui_object_t* entry = add_object(context, context->screen, UI_OBJECT_SCREEN, "screen");
    if (entry) {
        s_screen_owner = context;
        add_touch_events(context->screen, entry);
    } else {
        lv_obj_delete(context->screen);
        context->screen = NULL;
    }
    ui_unlock();
    if (!entry)
        return luaL_error(state, "ui object limit reached");
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_load(lua_State* state) {
    ui_context_t* context = get_context(state);
    int id = (int)luaL_optinteger(state, 1, 0);
    ui_object_t* entry = id ? find_object(context, id) : NULL;
    lv_obj_t* screen = entry ? entry->object : context->screen;
    if (!screen)
        return luaL_error(state, "invalid screen");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    context->previous_screen = lv_screen_active();
    lv_screen_load(screen);
    begin_exclusive_input();
    ui_unlock();
    return 0;
}

static int l_rect(lua_State* state) {
    const int opts = widget_table_index(state);
    ui_context_t* context = get_context(state);
    const char* event_id = read_string(state, opts, "event_id", NULL);
    lv_obj_t* parent = read_parent(state, context, opts);
    int x = read_integer(state, opts, "x", 0);
    int y = read_integer(state, opts, "y", 0);
    int width = read_integer(state, opts, "width", LV_SIZE_CONTENT);
    int height = read_integer(state, opts, "height", LV_SIZE_CONTENT);
    uint32_t color = read_color(state, opts, "color", 0x30363d);
    int radius = read_integer(state, opts, "radius", 0);
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t* object = lv_obj_create(parent);
    apply_geometry(object, x, y, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    ui_object_t* entry = add_object(context, object, UI_OBJECT_RECT, event_id);
    if (!entry) {
        lv_obj_delete(object);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    apply_pointer_policy(object, entry, event_id);
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_circle(lua_State* state) {
    const int opts = widget_table_index(state);
    ui_context_t* context = get_context(state);
    lv_obj_t* parent = read_parent(state, context, opts);
    int x = read_integer(state, opts, "x", 0), y = read_integer(state, opts, "y", 0);
    int radius = read_integer(state, opts, "radius", 10);
    uint32_t color = read_color(state, opts, "color", 0xffffff);
    int opacity = read_integer(state, opts, "opacity", 255);
    const char* event_id = read_string(state, opts, "event_id", NULL);
    if (radius <= 0)
        return luaL_argerror(state, 1, "radius must be positive");
    if (opacity < 0 || opacity > 255)
        return luaL_argerror(state, 1, "opacity must be between 0 and 255");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t* object = lv_obj_create(parent);
    apply_geometry(object, x, y, radius * 2, radius * 2);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_opa(object, (lv_opa_t)opacity, 0);
    ui_object_t* entry = add_object(context, object, UI_OBJECT_CIRCLE, event_id);
    if (!entry) {
        lv_obj_delete(object);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    apply_pointer_policy(object, entry, event_id);
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static bool parse_points(lua_State* state, int table_index, lv_point_precise_t** points_out,
                         uint32_t* count_out) {
    lua_getfield(state, table_index, "points");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    size_t count = lua_rawlen(state, -1);
    if (count < 2 || count > 256) {
        lua_pop(state, 1);
        return false;
    }
    lv_point_precise_t* points = calloc(count, sizeof(*points));
    if (!points) {
        lua_pop(state, 1);
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        lua_rawgeti(state, -1, (lua_Integer)i + 1);
        if (!lua_istable(state, -1)) {
            free(points);
            lua_pop(state, 2);
            return false;
        }
        points[i].x = (lv_value_precise_t)read_integer(state, -1, "x", 0);
        points[i].y = (lv_value_precise_t)read_integer(state, -1, "y", 0);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    *points_out = points;
    *count_out = (uint32_t)count;
    return true;
}

static int l_line(lua_State* state) {
    const int opts = widget_table_index(state);
    ui_context_t* context = get_context(state);
    lv_obj_t* parent = read_parent(state, context, opts);
    lv_point_precise_t* points = NULL;
    uint32_t count = 0;
    if (!parse_points(state, opts, &points, &count))
        return luaL_error(state, "points must contain at least two {x,y} entries");
    uint32_t color = read_color(state, opts, "color", 0xffffff);
    int width = read_integer(state, opts, "width", 2);
    lua_getfield(state, opts, "rounded");
    bool rounded = lua_toboolean(state, -1);
    lua_pop(state, 1);
    const char* event_id = read_string(state, opts, "event_id", NULL);
    if (width <= 0) {
        free(points);
        return luaL_argerror(state, 1, "width must be positive");
    }
    if (!ui_lock()) {
        free(points);
        return luaL_error(state, "display lock failed");
    }
    lv_obj_t* object = lv_line_create(parent);
    lv_line_set_points(object, points, count);
    lv_obj_set_style_line_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(object, width, 0);
    lv_obj_set_style_line_rounded(object, rounded, 0);
    ui_object_t* entry = add_object(context, object, UI_OBJECT_LINE, event_id);
    if (!entry) {
        lv_obj_delete(object);
        free(points);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    entry->owned_data = points;
    apply_pointer_policy(object, entry, event_id);
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_arc(lua_State* state) {
    const int opts = widget_table_index(state);
    ui_context_t* context = get_context(state);
    lv_obj_t* parent = read_parent(state, context, opts);
    int x = read_integer(state, opts, "x", 0), y = read_integer(state, opts, "y", 0),
        w = read_integer(state, opts, "width", 80), h = read_integer(state, opts, "height", 80);
    int start = read_integer(state, opts, "start_angle", 0),
        end = read_integer(state, opts, "end_angle", 360),
        width = read_integer(state, opts, "line_width", 4);
    uint32_t color = read_color(state, opts, "color", 0xffffff);
    const char* event_id = read_string(state, opts, "event_id", NULL);
    if (w <= 0 || h <= 0 || width <= 0)
        return luaL_argerror(state, 1, "arc width, height, and line_width must be positive");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t* object = lv_arc_create(parent);
    apply_geometry(object, x, y, w, h);
    lv_arc_set_angles(object, start, end);
    lv_obj_set_style_arc_color(object, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(object, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_KNOB);
    ui_object_t* entry = add_object(context, object, UI_OBJECT_ARC, event_id);
    if (!entry) {
        lv_obj_delete(object);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    apply_pointer_policy(object, entry, event_id);
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_image(lua_State* state) {
    const int opts = widget_table_index(state);
    ui_context_t* context = get_context(state);
    const char* src = read_string(state, opts, "src", NULL);
    if (!src)
        return luaL_error(state, "image src is required");
    lv_obj_t* parent = read_parent(state, context, opts);
    int x = read_integer(state, opts, "x", 0), y = read_integer(state, opts, "y", 0),
        rotation = read_integer(state, opts, "rotation", 0),
        scale = read_integer(state, opts, "scale", 256),
        opacity = read_integer(state, opts, "opacity", 255);
    int pivot_x = read_integer(state, opts, "pivot_x", 0),
        pivot_y = read_integer(state, opts, "pivot_y", 0),
        offset_x = read_integer(state, opts, "offset_x", 0),
        offset_y = read_integer(state, opts, "offset_y", 0);
    const char* event_id = read_string(state, opts, "event_id", NULL);
    if (scale <= 0)
        return luaL_argerror(state, 1, "scale must be positive");
    if (opacity < 0 || opacity > 255)
        return luaL_argerror(state, 1, "opacity must be between 0 and 255");
    char* owned = strdup(src);
    if (!owned)
        return luaL_error(state, "image source allocation failed");
    if (owned[0] == '/') {
        size_t len = strlen(owned);
        char* mapped = malloc(len + 3);
        if (!mapped) {
            free(owned);
            return luaL_error(state, "image source allocation failed");
        }
        snprintf(mapped, len + 3, "L:%s", owned);
        free(owned);
        owned = mapped;
    }
    if (!ui_lock()) {
        free(owned);
        return luaL_error(state, "display lock failed");
    }
    register_native_fs();
    lv_obj_t* object = lv_image_create(parent);
    lv_image_set_src(object, owned);
    lv_obj_set_pos(object, x, y);
    lv_image_set_rotation(object, rotation * 10);
    lv_image_set_scale(object, (uint32_t)scale);
    lv_image_set_pivot(object, pivot_x, pivot_y);
    lv_image_set_offset_x(object, offset_x);
    lv_image_set_offset_y(object, offset_y);
    lv_obj_set_style_opa(object, (lv_opa_t)opacity, 0);
    ui_object_t* entry = add_object(context, object, UI_OBJECT_IMAGE, event_id);
    if (!entry) {
        lv_obj_delete(object);
        free(owned);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    entry->owned_data = owned;
    apply_pointer_policy(object, entry, event_id);
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_label(lua_State* state) {
    const int opts = widget_table_index(state);
    ui_context_t* context = get_context(state);
    const char* text = read_string(state, opts, "text", "");
    lv_obj_t* parent = read_parent(state, context, opts);
    int x = read_integer(state, opts, "x", 0);
    int y = read_integer(state, opts, "y", 0);
    int width = read_integer(state, opts, "width", LV_SIZE_CONTENT);
    uint32_t color = read_color(state, opts, "color", 0xffffff);
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t* object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    if (width != LV_SIZE_CONTENT)
        lv_obj_set_width(object, width);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    ui_object_t* entry = add_object(context, object, UI_OBJECT_LABEL, NULL);
    if (!entry) {
        lv_obj_delete(object);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_button(lua_State* state) {
    const int opts = widget_table_index(state);
    ui_context_t* context = get_context(state);
    const char* text = read_string(state, opts, "text", "Button");
    const char* event_id = read_string(state, opts, "event_id", "button");
    lv_obj_t* parent = read_parent(state, context, opts);
    int x = read_integer(state, opts, "x", 0);
    int y = read_integer(state, opts, "y", 0);
    int width = read_integer(state, opts, "width", LV_SIZE_CONTENT);
    int height = read_integer(state, opts, "height", LV_SIZE_CONTENT);
    uint32_t color = read_color(state, opts, "color", 0x2563eb);
    uint32_t text_color = read_color(state, opts, "text_color", 0xffffff);
    int radius = read_integer(state, opts, "radius", 6);
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t* button = lv_button_create(parent);
    apply_geometry(button, x, y, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_radius(button, radius, 0);
    ui_object_t* entry = add_object(context, button, UI_OBJECT_BUTTON, event_id);
    if (!entry) {
        lv_obj_delete(button);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    add_touch_events(button, entry);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label);
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_set_text(lua_State* state) {
    ui_context_t* context = get_context(state);
    ui_object_t* entry = find_object(context, (int)luaL_checkinteger(state, 1));
    const char* text = luaL_checkstring(state, 2);
    if (!entry || entry->type != UI_OBJECT_LABEL)
        return luaL_error(state, "object is not a label");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_label_set_text(entry->object, text);
    ui_unlock();
    return 0;
}

static int l_screen_size(lua_State* state) {
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_display_t* display = lv_display_get_default();
    int32_t width = display ? lv_display_get_horizontal_resolution(display) : 0;
    int32_t height = display ? lv_display_get_vertical_resolution(display) : 0;
    ui_unlock();
    if (!display || width <= 0 || height <= 0)
        return luaL_error(state, "display is not available");
    lua_pushinteger(state, width);
    lua_pushinteger(state, height);
    return 2;
}

static int l_update(lua_State* state) {
    ui_context_t* context = get_context(state);
    ui_object_t* entry = find_object(context, (int)luaL_checkinteger(state, 1));
    luaL_checktype(state, 2, LUA_TTABLE);
    if (!entry)
        return luaL_error(state, "invalid UI object");

    lua_getfield(state, 2, "x");
    bool has_x = lua_isinteger(state, -1);
    int x = has_x ? (int)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);
    lua_getfield(state, 2, "y");
    bool has_y = lua_isinteger(state, -1);
    int y = has_y ? (int)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);
    if (has_x || has_y) {
        /* Position reads are performed after taking the display lock below. */
    }

    lua_getfield(state, 2, "width");
    bool has_width = lua_isinteger(state, -1);
    int width = has_width ? (int)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);
    lua_getfield(state, 2, "height");
    bool has_height = lua_isinteger(state, -1);
    int height = has_height ? (int)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);

    lua_getfield(state, 2, "color");
    bool has_color = lua_isinteger(state, -1);
    uint32_t color = has_color ? (uint32_t)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);

    lua_getfield(state, 2, "hidden");
    bool has_hidden = lua_isboolean(state, -1);
    bool hidden = has_hidden && lua_toboolean(state, -1);
    lua_pop(state, 1);

    lua_getfield(state, 2, "opacity");
    bool has_opacity = lua_isinteger(state, -1);
    int opacity = has_opacity ? (int)lua_tointeger(state, -1) : 255;
    lua_pop(state, 1);

    lua_getfield(state, 2, "z");
    bool has_z = lua_isinteger(state, -1);
    int z = has_z ? (int)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);

    lua_getfield(state, 2, "src");
    bool src_present = !lua_isnil(state, -1);
    bool has_src = lua_isstring(state, -1);
    const char* src = has_src ? lua_tostring(state, -1) : NULL;
    lua_pop(state, 1);
    if (src_present && (!has_src || entry->type != UI_OBJECT_IMAGE))
        return luaL_error(state, "src must be a string and can only update an image");

    lua_getfield(state, 2, "points");
    bool has_points = !lua_isnil(state, -1);
    lua_pop(state, 1);
    lv_point_precise_t* points = NULL;
    uint32_t point_count = 0;
    if (has_points &&
        (entry->type != UI_OBJECT_LINE || !parse_points(state, 2, &points, &point_count)))
        return luaL_error(state,
                          "points can only update a line and need at least two {x,y} entries");

    int rotation = read_integer(state, 2, "rotation", 0);
    lua_getfield(state, 2, "rotation");
    bool has_rotation = lua_isinteger(state, -1);
    lua_pop(state, 1);
    int scale = read_integer(state, 2, "scale", 256);
    lua_getfield(state, 2, "scale");
    bool has_scale = lua_isinteger(state, -1);
    lua_pop(state, 1);
    int pivot_x = read_integer(state, 2, "pivot_x", 0),
        pivot_y = read_integer(state, 2, "pivot_y", 0);
    lua_getfield(state, 2, "pivot_x");
    bool has_pivot_x = lua_isinteger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 2, "pivot_y");
    bool has_pivot_y = lua_isinteger(state, -1);
    lua_pop(state, 1);
    int offset_x = read_integer(state, 2, "offset_x", 0),
        offset_y = read_integer(state, 2, "offset_y", 0);
    lua_getfield(state, 2, "offset_x");
    bool has_offset_x = lua_isinteger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 2, "offset_y");
    bool has_offset_y = lua_isinteger(state, -1);
    lua_pop(state, 1);
    int start_angle = read_integer(state, 2, "start_angle", 0),
        end_angle = read_integer(state, 2, "end_angle", 360);
    lua_getfield(state, 2, "start_angle");
    bool has_start_angle = lua_isinteger(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, 2, "end_angle");
    bool has_end_angle = lua_isinteger(state, -1);
    lua_pop(state, 1);
    int line_width = read_integer(state, 2, "line_width", 1);
    lua_getfield(state, 2, "line_width");
    bool has_line_width = lua_isinteger(state, -1);
    lua_pop(state, 1);

    if ((has_rotation || has_scale || has_pivot_x || has_pivot_y || has_offset_x || has_offset_y) &&
        entry->type != UI_OBJECT_IMAGE) {
        free(points);
        return luaL_error(state, "rotation, scale, pivot, and offset apply only to images");
    }
    if ((has_start_angle || has_end_angle) && entry->type != UI_OBJECT_ARC) {
        free(points);
        return luaL_error(state, "angle fields apply only to arcs");
    }
    if (has_line_width && entry->type != UI_OBJECT_ARC && entry->type != UI_OBJECT_LINE) {
        free(points);
        return luaL_error(state, "line_width applies only to arcs or lines");
    }
    if (has_opacity && (opacity < 0 || opacity > 255)) {
        free(points);
        return luaL_argerror(state, 2, "opacity must be between 0 and 255");
    }
    if (has_scale && scale <= 0) {
        free(points);
        return luaL_argerror(state, 2, "scale must be positive");
    }
    if (has_line_width && line_width <= 0) {
        free(points);
        return luaL_argerror(state, 2, "line_width must be positive");
    }

    lua_getfield(state, 2, "text");
    bool has_text = !lua_isnil(state, -1);
    if (has_text && (entry->type != UI_OBJECT_LABEL || !lua_isstring(state, -1))) {
        lua_pop(state, 1);
        free(points);
        return luaL_error(state, "text must be a string and can only update a label");
    }
    const char* text = has_text ? lua_tostring(state, -1) : NULL;
    lua_pop(state, 1);

    if (!ui_lock()) {
        free(points);
        return luaL_error(state, "display lock failed");
    }

    if (has_x || has_y) {
        if (!has_x)
            x = lv_obj_get_x(entry->object);
        if (!has_y)
            y = lv_obj_get_y(entry->object);
        lv_obj_set_pos(entry->object, x, y);
    }

    if (has_width || has_height) {
        if (!has_width)
            width = lv_obj_get_width(entry->object);
        if (!has_height)
            height = lv_obj_get_height(entry->object);
        lv_obj_set_size(entry->object, width, height);
    }

    if (has_color) {
        if (entry->type == UI_OBJECT_LABEL)
            lv_obj_set_style_text_color(entry->object, lv_color_hex(color), 0);
        else if (entry->type == UI_OBJECT_LINE)
            lv_obj_set_style_line_color(entry->object, lv_color_hex(color), 0);
        else if (entry->type == UI_OBJECT_ARC)
            lv_obj_set_style_arc_color(entry->object, lv_color_hex(color), LV_PART_INDICATOR);
        else
            lv_obj_set_style_bg_color(entry->object, lv_color_hex(color), 0);
    }

    if (has_hidden) {
        if (hidden)
            lv_obj_add_flag(entry->object, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(entry->object, LV_OBJ_FLAG_HIDDEN);
    }

    if (has_opacity)
        lv_obj_set_style_opa(entry->object, (lv_opa_t)opacity, 0);
    if (has_z)
        lv_obj_move_to_index(entry->object, z);
    if (has_src) {
        char* replacement = strdup(src);
        if (!replacement) {
            ui_unlock();
            return luaL_error(state, "image source allocation failed");
        }
        if (replacement[0] == '/') {
            size_t len = strlen(replacement);
            char* mapped = malloc(len + 3);
            if (!mapped) {
                free(replacement);
                ui_unlock();
                return luaL_error(state, "image source allocation failed");
            }
            snprintf(mapped, len + 3, "L:%s", replacement);
            free(replacement);
            replacement = mapped;
        }
        lv_image_set_src(entry->object, replacement);
        free_owned_data(entry);
        entry->owned_data = replacement;
    }

    if (has_points) {
        lv_line_set_points(entry->object, points, point_count);
        free_owned_data(entry);
        entry->owned_data = points;
        points = NULL;
    }
    if (has_rotation || has_scale || has_pivot_x || has_pivot_y || has_offset_x || has_offset_y) {
        if (has_rotation)
            lv_image_set_rotation(entry->object, rotation * 10);
        if (has_scale)
            lv_image_set_scale(entry->object, (uint32_t)scale);
        if (has_pivot_x || has_pivot_y) {
            lv_point_t pivot;
            lv_image_get_pivot(entry->object, &pivot);
            if (!has_pivot_x)
                pivot_x = pivot.x;
            if (!has_pivot_y)
                pivot_y = pivot.y;
            lv_image_set_pivot(entry->object, pivot_x, pivot_y);
        }
        if (has_offset_x)
            lv_image_set_offset_x(entry->object, offset_x);
        if (has_offset_y)
            lv_image_set_offset_y(entry->object, offset_y);
    }
    if (has_start_angle || has_end_angle || has_line_width) {
        if (entry->type == UI_OBJECT_ARC) {
            if (!has_start_angle)
                start_angle = lv_arc_get_angle_start(entry->object);
            if (!has_end_angle)
                end_angle = lv_arc_get_angle_end(entry->object);
            if (has_start_angle || has_end_angle)
                lv_arc_set_angles(entry->object, start_angle, end_angle);
            if (has_line_width)
                lv_obj_set_style_arc_width(entry->object, line_width, LV_PART_INDICATOR);
        } else if (entry->type == UI_OBJECT_LINE && has_line_width) {
            lv_obj_set_style_line_width(entry->object, line_width, 0);
        }
    }

    if (has_text)
        lv_label_set_text(entry->object, text);
    free(points);
    ui_unlock();
    return 0;
}

static bool object_is_descendant(lv_obj_t* object, lv_obj_t* ancestor) {
    for (lv_obj_t* current = object; current; current = lv_obj_get_parent(current)) {
        if (current == ancestor)
            return true;
    }
    return false;
}

static int l_delete(lua_State* state) {
    ui_context_t* context = get_context(state);
    ui_object_t* entry = find_object(context, (int)luaL_checkinteger(state, 1));
    if (!entry)
        return luaL_error(state, "invalid UI object");
    if (entry->type == UI_OBJECT_SCREEN)
        return luaL_error(state, "the owned screen cannot be deleted");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t* object = entry->object;
    for (size_t i = 0; i < UI_MAX_OBJECTS; ++i) {
        ui_object_t* candidate = &context->objects[i];
        if (candidate->object && object_is_descendant(candidate->object, object)) {
            free_owned_data(candidate);
            candidate->object = NULL;
        }
    }
    lv_obj_delete(object);
    ui_unlock();
    return 0;
}

static int l_poll_event(lua_State* state) {
    ui_context_t* context = get_context(state);
    lua_Integer requested_timeout = luaL_optinteger(state, 1, 0);
    if (requested_timeout < 0 || requested_timeout > UINT32_MAX) {
        return luaL_argerror(state, 1, "timeout must be between 0 and UINT32_MAX");
    }
    uint32_t timeout_ms = (uint32_t)requested_timeout;
    uint32_t waited = 0;
    ui_event_t event;
    while (true) {
        uint32_t slice =
            timeout_ms == 0 ? 0 : (timeout_ms - waited > 50 ? 50 : timeout_ms - waited);
        if (xQueueReceive(context->events, &event, pdMS_TO_TICKS(slice)) == pdTRUE)
            break;
        lua_runtime_check_abort(state);
        waited += slice;
        if (timeout_ms == 0 || waited >= timeout_ms) {
            lua_pushnil(state);
            return 1;
        }
    }
    lua_createtable(state, 0, 9);
    lua_pushinteger(state, event.object_id);
    lua_setfield(state, -2, "object");
    lua_pushstring(state, event.event_id);
    lua_setfield(state, -2, "id");
    lua_pushstring(state, event.type);
    lua_setfield(state, -2, "type");
    lua_pushinteger(state, event.x);
    lua_setfield(state, -2, "x");
    lua_pushinteger(state, event.y);
    lua_setfield(state, -2, "y");
    lua_pushinteger(state, event.dx);
    lua_setfield(state, -2, "dx");
    lua_pushinteger(state, event.dy);
    lua_setfield(state, -2, "dy");
    lua_pushinteger(state, event.time_ms);
    lua_setfield(state, -2, "time_ms");
    lua_pushinteger(state, 0);
    lua_setfield(state, -2, "pointer");
    return 1;
}

/*
 * Test-only event injection.  This does not expose LVGL objects or bypass the
 * normal Lua queue; it only lets a self-test produce the same plain event that
 * a touch callback would enqueue when no physical finger is available.
 */
static int l_test_inject_event(lua_State* state) {
    lua_runtime_exec_context_t* exec = lua_runtime_get_context(state);
    if (!exec || !(exec->capabilities & LUA_RUNTIME_CAP_UI_TEST))
        return luaL_error(state, "UI test capability is not granted to this Lua job");
    ui_context_t* context = get_context(state);
    int object_id = (int)luaL_checkinteger(state, 1);
    const char* type = luaL_checkstring(state, 2);
    ui_object_t* entry = find_object(context, object_id);
    if (!entry)
        return luaL_error(state, "invalid UI object");
    if (strcmp(type, "pressed") != 0 && strcmp(type, "moved") != 0 &&
        strcmp(type, "released") != 0 && strcmp(type, "lost") != 0 &&
        strcmp(type, "clicked") != 0 && strcmp(type, "changed") != 0)
        return luaL_argerror(state, 2, "invalid event type");

    ui_event_t event = {.object_id = entry->id};
    strlcpy(event.event_id, entry->event_id, sizeof(event.event_id));
    strlcpy(event.type, type, sizeof(event.type));
    event.x = (int16_t)luaL_optinteger(state, 3, 0);
    event.y = (int16_t)luaL_optinteger(state, 4, 0);
    event.dx = (int16_t)luaL_optinteger(state, 5, 0);
    event.dy = (int16_t)luaL_optinteger(state, 6, 0);
    event.time_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (xQueueSend(context->events, &event, 0) != pdTRUE) {
        ui_event_t discarded;
        xQueueReceive(context->events, &discarded, 0);
        if (xQueueSend(context->events, &event, 0) != pdTRUE)
            return luaL_error(state, "UI event queue is full");
    }
    return 0;
}

static int close_context(lua_State* state) {
    ui_context_t* context = lua_touserdata(state, 1);
    if (!context || context->closed)
        return 0;
    context->closed = true;
    if (ui_lock()) {
        for (size_t i = 0; i < UI_MAX_OBJECTS; ++i)
            free_owned_data(&context->objects[i]);
        if (context->screen && lv_obj_is_valid(context->screen)) {
            if (lv_screen_active() == context->screen && context->previous_screen &&
                lv_obj_is_valid(context->previous_screen)) {
                lv_screen_load(context->previous_screen);
            }
            lv_obj_delete(context->screen);
        }
        if (s_screen_owner == context)
            s_screen_owner = NULL;
        end_exclusive_input();
        ui_unlock();
    }
    if (context->events)
        vQueueDelete(context->events);
    context->events = NULL;
    context->screen = NULL;
    return 0;
}

int luaopen_ui(lua_State* state) {
    if (ui_lock()) {
        register_native_fs();
        ui_unlock();
    }
    if (luaL_newmetatable(state, UI_CONTEXT_METATABLE)) {
        lua_pushcfunction(state, close_context);
        lua_setfield(state, -2, "__gc");
    }
    lua_pop(state, 1);
    static const luaL_Reg functions[] = {
        {"screen", l_screen},
        {"screen_size", l_screen_size},
        {"load", l_load},
        {"rect", l_rect},
        {"circle", l_circle},
        {"line", l_line},
        {"arc", l_arc},
        {"image", l_image},
        {"label", l_label},
        {"button", l_button},
        {"set_text", l_set_text},
        {"update", l_update},
        {"delete", l_delete},
        {"poll_event", l_poll_event},
        {"_test_inject_event", l_test_inject_event},
        {NULL, NULL},
    };
    luaL_newlib(state, functions);
    return 1;
}
