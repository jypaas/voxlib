/*
 * vox_http_mime.c - HTTP MIME 类型查询与注册实现
 */

#include "vox_http_mime.h"
#include <string.h>

/* 内置扩展名 -> MIME 表（小写扩展名） */
typedef struct {
    const char* ext;
    const char* mime;
} vox_http_mime_entry_t;

static const vox_http_mime_entry_t s_builtin[] = {
    /* text */
    { "html", "text/html" },
    { "htm",  "text/html" },
    { "css",  "text/css" },
    { "js",   "application/javascript" },
    { "json", "application/json" },
    { "xml",  "application/xml" },
    { "txt",  "text/plain" },
    { "csv",  "text/csv" },
    { "rtf",  "text/rtf" },
    { "md",   "text/markdown" },
    { "yaml", "text/yaml" },
    { "yml",  "text/yaml" },
    /* application */
    { "pdf",  "application/pdf" },
    { "zip",  "application/zip" },
    { "tar",  "application/x-tar" },
    { "gz",   "application/gzip" },
    { "7z",   "application/x-7z-compressed" },
    { "rar",  "application/vnd.rar" },
    { "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
    { "xls",  "application/vnd.ms-excel" },
    { "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
    { "doc",  "application/msword" },
    { "pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
    { "ppt",  "application/vnd.ms-powerpoint" },
    { "odt",  "application/vnd.oasis.opendocument.text" },
    { "ods",  "application/vnd.oasis.opendocument.spreadsheet" },
    { "odp",  "application/vnd.oasis.opendocument.presentation" },
    { "rss",  "application/rss+xml" },
    { "atom", "application/atom+xml" },
    { "wasm", "application/wasm" },
    { "map",  "application/json" }, /* source map */
    /* image */
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "svg",  "image/svg+xml" },
    { "ico",  "image/x-icon" },
    { "webp", "image/webp" },
    { "avif", "image/avif" },
    { "bmp",  "image/bmp" },
    { "tiff", "image/tiff" },
    { "tif",  "image/tiff" },
    /* audio */
    { "mp3",  "audio/mpeg" },
    { "wav",  "audio/wav" },
    { "ogg",  "audio/ogg" },
    { "m4a",  "audio/mp4" },
    { "aac",  "audio/aac" },
    { "flac", "audio/flac" },
    { "weba", "audio/webm" },
    /* video */
    { "mp4",  "video/mp4" },
    { "webm", "video/webm" },
    { "avi",  "video/x-msvideo" },
    { "mov",  "video/quicktime" },
    { "mkv",  "video/x-matroska" },
    { "m4v",  "video/x-m4v" },
    { "ogv",  "video/ogg" },
    /* font */
    { "woff", "font/woff" },
    { "woff2","font/woff2" },
    { "ttf",  "font/ttf" },
    { "otf",  "font/otf" },
    { "eot",  "application/vnd.ms-fontobject" },
};

#define BUILTIN_COUNT (sizeof(s_builtin) / sizeof(s_builtin[0]))

#define VOX_HTTP_MIME_CUSTOM_MAX 32
#define VOX_HTTP_MIME_EXT_MAX    16
#define VOX_HTTP_MIME_TYPE_MAX  80

typedef struct {
    char ext[VOX_HTTP_MIME_EXT_MAX];
    char mime[VOX_HTTP_MIME_TYPE_MAX];
} vox_http_mime_custom_t;

static vox_http_mime_custom_t s_custom[VOX_HTTP_MIME_CUSTOM_MAX];
static size_t s_custom_count;

/* 扩展名比较（不区分大小写），ext 长度为 ext_len，literal 为小写 NUL 结尾 */
static int ext_eq(const char* ext, size_t ext_len, const char* literal) {
    size_t i = 0;
    for (; i < ext_len && literal[i] != '\0'; i++) {
        char c = (char)(unsigned char)ext[i];
        if (c >= 'A' && c <= 'Z') c += (char)32;
        if (c != literal[i]) return 0;
    }
    return (literal[i] == '\0' && i == ext_len) ? 1 : 0;
}

const char* vox_http_mime_from_path(const char* path, size_t path_len) {
    if (!path || path_len == 0) return VOX_HTTP_MIME_DEFAULT;
    const char* dot = NULL;
    for (size_t i = path_len; i > 0; i--) {
        if (path[i - 1] == '.') { dot = path + (i - 1); break; }
        if (path[i - 1] == '/' || path[i - 1] == '\\') break;
    }
    if (!dot || dot >= path + path_len) return VOX_HTTP_MIME_DEFAULT;
    const char* ext = dot + 1;
    size_t ext_len = path_len - (size_t)(ext - path);
    return vox_http_mime_from_ext(ext, ext_len);
}

const char* vox_http_mime_from_ext(const char* ext, size_t ext_len) {
    if (!ext || ext_len == 0) return VOX_HTTP_MIME_DEFAULT;
    if (ext_len >= VOX_HTTP_MIME_EXT_MAX) return VOX_HTTP_MIME_DEFAULT;

    /* 先查自定义表 */
    for (size_t i = 0; i < s_custom_count; i++) {
        size_t n = 0;
        while (n < ext_len && n < VOX_HTTP_MIME_EXT_MAX - 1 && s_custom[i].ext[n] != '\0') n++;
        if (n != ext_len) continue;
        int eq = 1;
        for (size_t k = 0; k < ext_len; k++) {
            char c = (char)(unsigned char)ext[k];
            if (c >= 'A' && c <= 'Z') c += (char)32;
            if (c != s_custom[i].ext[k]) { eq = 0; break; }
        }
        if (eq) return s_custom[i].mime;
    }

    /* 再查内置表 */
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (ext_eq(ext, ext_len, s_builtin[i].ext))
            return s_builtin[i].mime;
    }
    return VOX_HTTP_MIME_DEFAULT;
}

int vox_http_mime_register(const char* ext, const char* mime_type) {
    if (!ext || !mime_type) return -1;
    size_t ext_len = strlen(ext);
    size_t mime_len = strlen(mime_type);
    if (ext_len == 0 || ext_len >= VOX_HTTP_MIME_EXT_MAX || mime_len >= VOX_HTTP_MIME_TYPE_MAX)
        return -1;

    /* 若已存在同扩展名，则覆盖 */
    for (size_t i = 0; i < s_custom_count; i++) {
        if (strcasecmp(s_custom[i].ext, ext) == 0) {
            for (size_t k = 0; k < ext_len && k < VOX_HTTP_MIME_EXT_MAX - 1; k++)
                s_custom[i].ext[k] = (char)(unsigned char)(ext[k] >= 'A' && ext[k] <= 'Z' ? ext[k] + 32 : ext[k]);
            s_custom[i].ext[ext_len] = '\0';
            for (size_t k = 0; k <= mime_len && k < VOX_HTTP_MIME_TYPE_MAX; k++)
                s_custom[i].mime[k] = mime_type[k];
            return 0;
        }
    }

    if (s_custom_count >= VOX_HTTP_MIME_CUSTOM_MAX) return -1;
    vox_http_mime_custom_t* e = &s_custom[s_custom_count++];
    for (size_t k = 0; k < ext_len && k < VOX_HTTP_MIME_EXT_MAX - 1; k++)
        e->ext[k] = (char)(unsigned char)(ext[k] >= 'A' && ext[k] <= 'Z' ? ext[k] + 32 : ext[k]);
    e->ext[ext_len] = '\0';
    for (size_t k = 0; k <= mime_len && k < VOX_HTTP_MIME_TYPE_MAX; k++)
        e->mime[k] = mime_type[k];
    return 0;
}
