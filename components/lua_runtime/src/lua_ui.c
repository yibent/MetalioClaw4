#include "lua_ui.h"

#include <stdlib.h>
#include <string.h>

#include "esp_lv_adapter.h"
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

typedef enum {
    UI_OBJECT_NONE = 0,
    UI_OBJECT_SCREEN,
    UI_OBJECT_RECT,
    UI_OBJECT_LABEL,
    UI_OBJECT_BUTTON,
} ui_object_type_t;

typedef struct ui_context ui_context_t;

typedef struct {
    int id;
    ui_object_type_t type;
    lv_obj_t *object;
    ui_context_t *context;
    char event_id[UI_EVENT_ID_LENGTH];
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
    lv_obj_t *screen;
    lv_obj_t *previous_screen;
    ui_object_t objects[UI_MAX_OBJECTS];
    int next_id;
    bool closed;
    bool touch_active;
    int16_t touch_x;
    int16_t touch_y;
};

static const char kContextRegistryKey;
static ui_context_t *s_screen_owner;

static bool ui_lock(void) { return esp_lv_adapter_lock(-1) == ESP_OK; }

static void ui_unlock(void) { esp_lv_adapter_unlock(); }

static uint32_t read_color(lua_State *state, int table_index, const char *field,
                           uint32_t default_value) {
    lua_getfield(state, table_index, field);
    uint32_t value = lua_isinteger(state, -1) ? (uint32_t)lua_tointeger(state, -1) : default_value;
    lua_pop(state, 1);
    return value;
}

static int read_integer(lua_State *state, int table_index, const char *field, int default_value) {
    lua_getfield(state, table_index, field);
    int value = lua_isinteger(state, -1) ? (int)lua_tointeger(state, -1) : default_value;
    lua_pop(state, 1);
    return value;
}

static const char *read_string(lua_State *state, int table_index, const char *field,
                               const char *default_value) {
    lua_getfield(state, table_index, field);
    const char *value = lua_isstring(state, -1) ? lua_tostring(state, -1) : default_value;
    lua_pop(state, 1);
    return value;
}

static ui_context_t *get_context(lua_State *state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kContextRegistryKey);
    ui_context_t *context = lua_touserdata(state, -1);
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

static ui_object_t *find_object(ui_context_t *context, int id) {
    for (size_t i = 0; i < UI_MAX_OBJECTS; ++i) {
        if (context->objects[i].id == id && context->objects[i].object)
            return &context->objects[i];
    }
    return NULL;
}

static ui_object_t *add_object(ui_context_t *context, lv_obj_t *object, ui_object_type_t type,
                               const char *event_id) {
    for (size_t i = 0; i < UI_MAX_OBJECTS; ++i) {
        if (!context->objects[i].object) {
            ui_object_t *entry = &context->objects[i];
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

static const char *event_type_name(lv_event_code_t code) {
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

static void object_event_callback(lv_event_t *event) {
    ui_object_t *entry = lv_event_get_user_data(event);
    if (!entry || !entry->context || !entry->context->events)
        return;
    lv_event_code_t code = lv_event_get_code(event);
    ui_event_t queued = {.object_id = entry->id};
    strlcpy(queued.event_id, entry->event_id, sizeof(queued.event_id));
    strlcpy(queued.type, event_type_name(lv_event_get_code(event)), sizeof(queued.type));
    lv_indev_t *indev = lv_event_get_indev(event);
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

static void add_touch_events(lv_obj_t *object, ui_object_t *entry) {
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_PRESSED, entry);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_PRESSING, entry);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_PRESS_LOST, entry);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_RELEASED, entry);
    lv_obj_add_event_cb(object, object_event_callback, LV_EVENT_CLICKED, entry);
}

static void apply_geometry(lv_obj_t *object, int x, int y, int width, int height) {
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
}

static lv_obj_t *read_parent(lua_State *state, ui_context_t *context, int table_index) {
    lua_getfield(state, table_index, "parent");
    int parent_id = lua_isinteger(state, -1) ? (int)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);
    if (parent_id) {
        ui_object_t *parent = find_object(context, parent_id);
        if (!parent)
            luaL_error(state, "invalid parent object");
        return parent->object;
    }
    if (!context->screen)
        luaL_error(state, "ui.screen() must be called first");
    return context->screen;
}

static int l_screen(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    ui_context_t *context = get_context(state);
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
    lv_obj_set_style_bg_color(context->screen, lv_color_hex(background), 0);
    ui_object_t *entry = add_object(context, context->screen, UI_OBJECT_SCREEN, "screen");
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

static int l_load(lua_State *state) {
    ui_context_t *context = get_context(state);
    int id = (int)luaL_optinteger(state, 1, 0);
    ui_object_t *entry = id ? find_object(context, id) : NULL;
    lv_obj_t *screen = entry ? entry->object : context->screen;
    if (!screen)
        return luaL_error(state, "invalid screen");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    context->previous_screen = lv_screen_active();
    lv_screen_load(screen);
    ui_unlock();
    return 0;
}

static int l_rect(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    ui_context_t *context = get_context(state);
    const char *event_id = read_string(state, 1, "event_id", NULL);
    lv_obj_t *parent = read_parent(state, context, 1);
    int x = read_integer(state, 1, "x", 0);
    int y = read_integer(state, 1, "y", 0);
    int width = read_integer(state, 1, "width", LV_SIZE_CONTENT);
    int height = read_integer(state, 1, "height", LV_SIZE_CONTENT);
    uint32_t color = read_color(state, 1, "color", 0x30363d);
    int radius = read_integer(state, 1, "radius", 0);
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t *object = lv_obj_create(parent);
    apply_geometry(object, x, y, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    ui_object_t *entry = add_object(context, object, UI_OBJECT_RECT, event_id);
    if (!entry) {
        lv_obj_delete(object);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    if (event_id) {
        add_touch_events(object, entry);
    }
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_label(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    ui_context_t *context = get_context(state);
    const char *text = read_string(state, 1, "text", "");
    lv_obj_t *parent = read_parent(state, context, 1);
    int x = read_integer(state, 1, "x", 0);
    int y = read_integer(state, 1, "y", 0);
    int width = read_integer(state, 1, "width", LV_SIZE_CONTENT);
    uint32_t color = read_color(state, 1, "color", 0xffffff);
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    if (width != LV_SIZE_CONTENT)
        lv_obj_set_width(object, width);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    ui_object_t *entry = add_object(context, object, UI_OBJECT_LABEL, NULL);
    if (!entry) {
        lv_obj_delete(object);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_button(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    ui_context_t *context = get_context(state);
    const char *text = read_string(state, 1, "text", "Button");
    const char *event_id = read_string(state, 1, "event_id", "button");
    lv_obj_t *parent = read_parent(state, context, 1);
    int x = read_integer(state, 1, "x", 0);
    int y = read_integer(state, 1, "y", 0);
    int width = read_integer(state, 1, "width", LV_SIZE_CONTENT);
    int height = read_integer(state, 1, "height", LV_SIZE_CONTENT);
    uint32_t color = read_color(state, 1, "color", 0x2563eb);
    uint32_t text_color = read_color(state, 1, "text_color", 0xffffff);
    int radius = read_integer(state, 1, "radius", 6);
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t *button = lv_button_create(parent);
    apply_geometry(button, x, y, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_radius(button, radius, 0);
    ui_object_t *entry = add_object(context, button, UI_OBJECT_BUTTON, event_id);
    if (!entry) {
        lv_obj_delete(button);
        ui_unlock();
        return luaL_error(state, "ui object limit reached");
    }
    add_touch_events(button, entry);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
    lv_obj_center(label);
    ui_unlock();
    lua_pushinteger(state, entry->id);
    return 1;
}

static int l_set_text(lua_State *state) {
    ui_context_t *context = get_context(state);
    ui_object_t *entry = find_object(context, (int)luaL_checkinteger(state, 1));
    const char *text = luaL_checkstring(state, 2);
    if (!entry || entry->type != UI_OBJECT_LABEL)
        return luaL_error(state, "object is not a label");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_label_set_text(entry->object, text);
    ui_unlock();
    return 0;
}

static int l_screen_size(lua_State *state) {
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_display_t *display = lv_display_get_default();
    int32_t width = display ? lv_display_get_horizontal_resolution(display) : 0;
    int32_t height = display ? lv_display_get_vertical_resolution(display) : 0;
    ui_unlock();
    if (!display || width <= 0 || height <= 0)
        return luaL_error(state, "display is not available");
    lua_pushinteger(state, width);
    lua_pushinteger(state, height);
    return 2;
}

static int l_update(lua_State *state) {
    ui_context_t *context = get_context(state);
    ui_object_t *entry = find_object(context, (int)luaL_checkinteger(state, 1));
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
        if (!has_x)
            x = lv_obj_get_x(entry->object);
        if (!has_y)
            y = lv_obj_get_y(entry->object);
        lv_obj_set_pos(entry->object, x, y);
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

    lua_getfield(state, 2, "text");
    bool has_text = !lua_isnil(state, -1);
    if (has_text && (entry->type != UI_OBJECT_LABEL || !lua_isstring(state, -1))) {
        lua_pop(state, 1);
        return luaL_error(state, "text must be a string and can only update a label");
    }
    const char *text = has_text ? lua_tostring(state, -1) : NULL;
    lua_pop(state, 1);

    if (!ui_lock())
        return luaL_error(state, "display lock failed");

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
        else
            lv_obj_set_style_bg_color(entry->object, lv_color_hex(color), 0);
    }

    if (has_hidden) {
        if (hidden)
            lv_obj_add_flag(entry->object, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(entry->object, LV_OBJ_FLAG_HIDDEN);
    }

    if (has_text)
        lv_label_set_text(entry->object, text);
    ui_unlock();
    return 0;
}

static bool object_is_descendant(lv_obj_t *object, lv_obj_t *ancestor) {
    for (lv_obj_t *current = object; current; current = lv_obj_get_parent(current)) {
        if (current == ancestor)
            return true;
    }
    return false;
}

static int l_delete(lua_State *state) {
    ui_context_t *context = get_context(state);
    ui_object_t *entry = find_object(context, (int)luaL_checkinteger(state, 1));
    if (!entry)
        return luaL_error(state, "invalid UI object");
    if (entry->type == UI_OBJECT_SCREEN)
        return luaL_error(state, "the owned screen cannot be deleted");
    if (!ui_lock())
        return luaL_error(state, "display lock failed");
    lv_obj_t *object = entry->object;
    for (size_t i = 0; i < UI_MAX_OBJECTS; ++i) {
        ui_object_t *candidate = &context->objects[i];
        if (candidate->object && object_is_descendant(candidate->object, object))
            candidate->object = NULL;
    }
    lv_obj_delete(object);
    ui_unlock();
    return 0;
}

static int l_poll_event(lua_State *state) {
    ui_context_t *context = get_context(state);
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

static int close_context(lua_State *state) {
    ui_context_t *context = lua_touserdata(state, 1);
    if (!context || context->closed)
        return 0;
    context->closed = true;
    if (ui_lock()) {
        if (context->screen && lv_obj_is_valid(context->screen)) {
            if (lv_screen_active() == context->screen && context->previous_screen &&
                lv_obj_is_valid(context->previous_screen)) {
                lv_screen_load(context->previous_screen);
            }
            lv_obj_delete(context->screen);
        }
        if (s_screen_owner == context)
            s_screen_owner = NULL;
        ui_unlock();
    }
    if (context->events)
        vQueueDelete(context->events);
    context->events = NULL;
    context->screen = NULL;
    return 0;
}

int luaopen_ui(lua_State *state) {
    if (luaL_newmetatable(state, UI_CONTEXT_METATABLE)) {
        lua_pushcfunction(state, close_context);
        lua_setfield(state, -2, "__gc");
    }
    lua_pop(state, 1);
    static const luaL_Reg functions[] = {
        {"screen", l_screen},         {"screen_size", l_screen_size},
        {"load", l_load},             {"rect", l_rect},
        {"label", l_label},           {"button", l_button},
        {"set_text", l_set_text},     {"update", l_update},
        {"delete", l_delete},         {"poll_event", l_poll_event},
        {NULL, NULL},
    };
    luaL_newlib(state, functions);
    return 1;
}
