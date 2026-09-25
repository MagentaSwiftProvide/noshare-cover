/*
 * noshare-cover — публичный API для других плагинов Hyprland.
 *
 * Зачем: плагин вроде оверлея окон (gloview) рисует превью окна с no_screen_share
 * у себя в кадре. noshare-cover закрывает в скриншере только само окно, а превью
 * утекло бы. Через этот API плагин сообщает «вот тут тоже закрой» — чёрным
 * или обложкой того самого окна.
 *
 * Подключение — только этот заголовок, линковать ничего не нужно:
 *
 *     #include "noshare_cover_api.h"
 *
 *     static noshare_cover_api g_nsc;          // в PLUGIN_INIT:
 *     if (noshare_cover_bind(&g_nsc) == 0)
 *         g_client = g_nsc.register_client("gloview");
 *
 *     // каждый кадр оверлея (или когда плитки сдвинулись), на каждый монитор:
 *     noshare_cover_rect r[] = {{x, y, w, h, rounding, window_address, NOSHARE_COVER_FILL_WINDOW}};
 *     g_nsc.set_rects(g_client, monitor_id, r, 1);
 *
 *     // оверлей закрылся:           g_nsc.set_rects(g_client, monitor_id, NULL, 0);
 *     // выгрузка своего плагина:    g_nsc.unregister_client(g_client); noshare_cover_unbind(&g_nsc);
 *
 * Координаты — глобальные логические пиксели раскладки (то же пространство, что
 * позиция и размер окна). noshare-cover сам вычтет позицию монитора, умножит на
 * scale и учтёт область захвата. Прямоугольники живут, пока их не заменят.
 *
 * Почему bind через dl_iterate_phdr: Hyprland грузит плагины с RTLD_LOCAL,
 * dlsym(RTLD_DEFAULT, ...) их символы не видит.
 */
#ifndef NOSHARE_COVER_API_H
#define NOSHARE_COVER_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOSHARE_COVER_API_VERSION 2

enum {
    NOSHARE_COVER_FILL_BLACK  = 0,
    /* Обложка окна `window` (по его правилам или глобальная). Нет окна/обложки — чёрным. */
    NOSHARE_COVER_FILL_WINDOW = 1,
};

typedef struct {
    double   x, y, w, h;
    double   rounding;
    uint64_t window; /* адрес окна, как `address` в `hyprctl clients -j`; 0 — нет */
    uint32_t fill;   /* NOSHARE_COVER_FILL_* */
} noshare_cover_rect;

typedef struct {
    uint32_t version; /* что сказал сам плагин */
    void*    handle;  /* RTLD_NOLOAD-ссылка, закрывается в noshare_cover_unbind */

    /* v2 */
    uint64_t (*register_client)(const char* name);
    void (*unregister_client)(uint64_t client);
    bool (*set_rects)(uint64_t client, int monitor_id, const noshare_cover_rect* rects, size_t count);
    bool (*clear_client_rects)(uint64_t client);

    /* v2, необязательно (NULL у старых сборок): «ухожу». Колбэк зовётся при выгрузке
     * noshare-cover, уже после снятия его хука renderMonitor: забудьте все указатели
     * отсюда, renderMonitor свободен. С колбэком можно сразу отпустить handle
     * (noshare_cover_drop_handle), чтобы не мешать выгрузке noshare-cover. */
    bool (*set_gone_callback)(uint64_t client, void (*cb)(void* user), void* user);

    /* v1, оставлены для совместимости */
    void (*clear_extra_rects)(void);
    void (*add_extra_rect)(int monitor_id, double x, double y, double w, double h, double rounding);
} noshare_cover_api;

#if defined(__linux__) && !defined(NOSHARE_COVER_NO_BINDER)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <link.h>
#include <string.h>

static int noshare_cover__find(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    if (!info->dlpi_name || !strstr(info->dlpi_name, "noshare-cover"))
        return 0;
    *(const char**)data = info->dlpi_name;
    return 1;
}

/*
 * Найти загруженный noshare-cover и привязать функции.
 * 0 — готово (v2 доступен); 1 — плагин не загружен; 2 — старая версия без v2
 * (v1-функции всё равно заполнены, если есть); 3 — ошибка dlopen.
 */
static inline int noshare_cover_bind(noshare_cover_api* api) {
    memset(api, 0, sizeof *api);

    const char* path = NULL;
    dl_iterate_phdr(noshare_cover__find, &path);
    if (!path)
        return 1;

    void* h = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (!h)
        return 3;
    api->handle = h;

#define NOSHARE_COVER__SYM(field, name) *(void**)(&api->field) = dlsym(h, name)
    NOSHARE_COVER__SYM(clear_extra_rects, "noshare_cover_clear_extra_rects");
    NOSHARE_COVER__SYM(add_extra_rect, "noshare_cover_add_extra_rect");

    uint32_t (*version)(void) = NULL;
    *(void**)(&version)       = dlsym(h, "noshare_cover_api_version");
    api->version              = version ? version() : 1;
    if (api->version < 2)
        return 2;

    NOSHARE_COVER__SYM(register_client, "noshare_cover_register_client");
    NOSHARE_COVER__SYM(unregister_client, "noshare_cover_unregister_client");
    NOSHARE_COVER__SYM(set_rects, "noshare_cover_set_rects");
    NOSHARE_COVER__SYM(clear_client_rects, "noshare_cover_clear_client_rects");
    NOSHARE_COVER__SYM(set_gone_callback, "noshare_cover_set_gone_callback");
#undef NOSHARE_COVER__SYM

    return (api->register_client && api->unregister_client && api->set_rects && api->clear_client_rects) ? 0 : 2;
}

/* Отпустить RTLD_NOLOAD-ссылку, сохранив указатели. Только вместе с set_gone_callback:
 * без неё после выгрузки noshare-cover указатели повиснут. Держать handle дольше
 * не надо — пока он открыт, dlclose не выгружает noshare-cover из памяти. */
static inline void noshare_cover_drop_handle(noshare_cover_api* api) {
    if (api->handle)
        dlclose(api->handle);
    api->handle = NULL;
}

static inline void noshare_cover_unbind(noshare_cover_api* api) {
    if (api->handle)
        dlclose(api->handle);
    memset(api, 0, sizeof *api);
}

#endif /* __linux__ */

#endif
