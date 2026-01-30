/*
 * vox.c - VoxLib 统一入口
 * 提供库级 API：初始化、反初始化、版本查询。
 */

#include "vox.h"

#define VOX_VERSION_STR "1.0.0"

static int g_ref_count;

void vox_init(void)
{
    if (g_ref_count == 0) {
        if (vox_socket_init() != 0) {
            return; /* socket 初始化失败，不增加引用计数 */
        }
    }
    ++g_ref_count;
}

void vox_fini(void)
{
    if (g_ref_count > 0) {
        --g_ref_count;
        if (g_ref_count == 0) {
            vox_socket_cleanup();
        }
    }
}

const char* vox_version(void)
{
    return VOX_VERSION_STR;
}
