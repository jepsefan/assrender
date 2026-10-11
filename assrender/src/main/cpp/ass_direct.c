#include "ass_direct.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>
#include <ass/ass.h>

#define LOG_TAG "assrender"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct AssDirectContext {
    int width;
    int height;
    float font_scale;

    ASS_Library *ass_library;
    ASS_Renderer *ass_renderer;
    ASS_Track *ass_track;
    int last_frame_visible;
    int frame_initialized;
};

AssDirectContext *ass_direct_init(int width, int height, float font_scale) {
    AssDirectContext *ctx = calloc(1, sizeof(AssDirectContext));
    if (!ctx) return NULL;

    ctx->width = width;
    ctx->height = height;
    ctx->font_scale = font_scale;

    ctx->ass_library = ass_library_init();
    if (!ctx->ass_library) {
        LOGE("Failed to init libass library");
        free(ctx);
        return NULL;
    }
    // Enable libass debug messages
    ass_set_message_cb(ctx->ass_library,
        (void (*)(int, const char *, va_list, void *))NULL, NULL);
    ass_set_extract_fonts(ctx->ass_library, 1);

    ctx->ass_renderer = ass_renderer_init(ctx->ass_library);
    if (!ctx->ass_renderer) {
        LOGE("Failed to init libass renderer");
        ass_library_done(ctx->ass_library);
        free(ctx);
        return NULL;
    }

    ass_set_storage_size(ctx->ass_renderer, width, height);
    ass_set_frame_size(ctx->ass_renderer, width, height);
    ass_set_font_scale(ctx->ass_renderer, (double)font_scale);
    // Android normally has no fontconfig provider. Supply an actual system
    // font file instead of relying on AUTODETECT, which can yield no glyphs.
    const char *font_path = "/system/fonts/Roboto-Regular.ttf";
    if (access(font_path, R_OK) != 0) font_path = "/system/fonts/NotoSans-Regular.ttf";
    if (access(font_path, R_OK) != 0) font_path = NULL;
    ass_set_fonts(ctx->ass_renderer, font_path, "sans-serif",
                  ASS_FONTPROVIDER_NONE, NULL, 1);
    LOGI("ASS font provider: NONE, default font path: %s", font_path ? font_path : "(missing)");

    LOGI("ass_direct initialized: %dx%d, font_scale=%.2f (with system fonts)", width, height, font_scale);
    return ctx;
}

int ass_direct_load_header(AssDirectContext *ctx, const char *header_data, int header_size) {
    if (!ctx || !header_data || header_size <= 0) return -1;

    // Free previous track if any
    if (ctx->ass_track) {
        ass_free_track(ctx->ass_track);
        ctx->ass_track = NULL;
    }

    ctx->frame_initialized = 0;
    ctx->ass_track = ass_new_track(ctx->ass_library);
    if (!ctx->ass_track) {
        LOGE("Failed to create ASS track");
        return -1;
    }

    ass_process_codec_private(ctx->ass_track, (char *)header_data, header_size);

    LOGI("Loaded ASS header: %d bytes, %d styles",
         header_size, ctx->ass_track->n_styles);
    return 0;
}

int ass_direct_load_script(AssDirectContext *ctx, const char *data, int size) {
    if (!ctx || !data || size <= 0) return -1;
    ASS_Track *track = ass_read_memory(ctx->ass_library, (char *)data, (size_t)size, NULL);
    if (!track) { LOGE("ASS script parsing failed, bytes=%d", size); return -1; }
    if (ctx->ass_track) ass_free_track(ctx->ass_track);
    ctx->ass_track = track;
    ctx->frame_initialized = 0;
    ctx->last_frame_visible = 0;
    LOGI("ASS script loaded: %d events, %d styles", track->n_events, track->n_styles);
    if (track->n_events > 0) {
        ASS_Event *e = &track->events[0];
        LOGI("First event: start=%lldms duration=%lldms style=%d",
             (long long)e->Start, (long long)e->Duration, e->Style);
    }
    return 0;
}

void ass_direct_add_font(AssDirectContext *ctx, const char *name,
                         const char *data, int data_size) {
    if (!ctx || !data || data_size <= 0) return;
    ass_add_font(ctx->ass_library, name, (char *)data, data_size);

    // Embedded fonts are registered with the libass library and remain available.

    LOGI("Added font: %s (%d bytes)", name ? name : "unnamed", data_size);
}

void ass_direct_process_chunk(AssDirectContext *ctx, const char *data, int size,
                              int64_t start_ms, int64_t duration_ms) {
    if (!ctx || !ctx->ass_track || !data || size <= 0) return;
    ass_process_chunk(ctx->ass_track, (char *)data, size, start_ms, duration_ms);
}

void ass_direct_process_data(AssDirectContext *ctx, const char *data, int size) {
    if (!ctx || !ctx->ass_track || !data || size <= 0) return;
    ass_process_data(ctx->ass_track, (char *)data, size);
}

int ass_direct_render(AssDirectContext *ctx, int64_t time_ms, uint8_t *out_pixels) {
    if (!ctx || !ctx->ass_track || !ctx->ass_renderer || !out_pixels)
        return 0;

    int changed = 0;
    ASS_Image *img = ass_render_frame(ctx->ass_renderer, ctx->ass_track,
                                      time_ms, &changed);

    // libass reports 0 when the image and positions are unchanged.
    // Preserve the previous bitmap and skip compositing/PNG encoding.
    if (ctx->frame_initialized && changed == 0) return ctx->last_frame_visible ? 2 : 0;
    ctx->frame_initialized = 1;
    ctx->last_frame_visible = img != NULL;

    // Clear output buffer
    memset(out_pixels, 0, ctx->width * ctx->height * 4);

    if (!img) {
        static int64_t last_empty_log_second = -1;
        if (time_ms / 1000 != last_empty_log_second) {
            last_empty_log_second = time_ms / 1000;
            int active = 0;
            int first_active_style = -1;
            for (int i = 0; i < ctx->ass_track->n_events; ++i) {
                ASS_Event *event = &ctx->ass_track->events[i];
                if (event->Start <= time_ms && time_ms < event->Start + event->Duration) {
                    ++active;
                    if (first_active_style < 0) first_active_style = event->Style;
                }
            }
            LOGI("ASS frame empty: time=%lldms events=%d active=%d first_active_style=%d",
                 (long long)time_ms, ctx->ass_track->n_events, active, first_active_style);
        }
        return 0;
    }

    // Composite all ASS_Image layers into the RGBA output buffer
    int has_content = 0;
    while (img) {
        if (img->w == 0 || img->h == 0) {
            img = img->next;
            continue;
        }

        has_content = 1;
        uint8_t r = (img->color >> 24) & 0xFF;
        uint8_t g = (img->color >> 16) & 0xFF;
        uint8_t b = (img->color >> 8) & 0xFF;
        uint8_t a = 255 - (img->color & 0xFF);

        uint8_t *src = img->bitmap;
        for (int y = 0; y < img->h; y++) {
            if (img->dst_y + y < 0 || img->dst_y + y >= ctx->height) {
                src += img->stride;
                continue;
            }
            for (int x = 0; x < img->w; x++) {
                const int pixel_x = img->dst_x + x;
                if (pixel_x < 0 || pixel_x >= ctx->width) continue;
                uint8_t *dst = out_pixels + ((img->dst_y + y) * ctx->width + pixel_x) * 4;
                uint8_t alpha = (uint8_t)(((uint32_t)src[x] * a) >> 8);
                if (alpha == 0) {
                    continue;
                }

                uint8_t dst_a = dst[3];
                if (dst_a == 0) {
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    dst[3] = alpha;
                } else {
                    uint32_t inv = 255 - alpha;
                    dst[0] = (uint8_t)((alpha * r + inv * dst[0]) / 255);
                    dst[1] = (uint8_t)((alpha * g + inv * dst[1]) / 255);
                    dst[2] = (uint8_t)((alpha * b + inv * dst[2]) / 255);
                    dst[3] = (uint8_t)(alpha + (inv * dst_a) / 255);
                }
            }
            src += img->stride;
        }
        img = img->next;
    }

    return has_content;
}

void ass_direct_flush(AssDirectContext *ctx) {
    if (!ctx || !ctx->ass_track) return;
    ass_flush_events(ctx->ass_track);
    ctx->frame_initialized = 0;
    ctx->last_frame_visible = 0;
    LOGI("Flushed ASS events");
}

void ass_direct_destroy(AssDirectContext *ctx) {
    if (!ctx) return;

    if (ctx->ass_track)
        ass_free_track(ctx->ass_track);
    if (ctx->ass_renderer)
        ass_renderer_done(ctx->ass_renderer);
    if (ctx->ass_library)
        ass_library_done(ctx->ass_library);

    free(ctx);
    LOGI("ass_direct destroyed");
}
