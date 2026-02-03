/*
 * vox_file.c - 跨平台文件操作实现
 * 提供统一的文件读写、文件信息、目录操作等接口
 */

#include "vox_file.h"
#include "vox_os.h"
#include "vox_mpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#ifdef VOX_OS_WINDOWS
    #include <direct.h>
    #include <sys/stat.h>
    #define stat _stat
    #ifndef S_ISDIR
        #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
    #endif
    #ifndef S_ISREG
        #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #endif
    #define access _access
    #define F_OK 0
    #define R_OK 4
    #define W_OK 2
    #define mkdir(path, mode) _mkdir(path)
    #define rmdir _rmdir
    #define unlink _unlink
    #define rename _rename
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <limits.h>
    #ifndef PATH_MAX
    #define PATH_MAX 4096
    #endif
    #include <sys/types.h>
    #include <dirent.h>
    #include <fcntl.h>
    #include <limits.h>
#endif

/* 文件结构 */
struct vox_file {
    vox_mpool_t* mpool;  /* 内存池指针 */
#ifdef VOX_OS_WINDOWS
    HANDLE handle;       /* Windows文件句柄 */
#else
    int fd;              /* POSIX文件描述符 */
#endif
    vox_file_mode_t mode; /* 打开模式 */
};

/* 打开文件 */
vox_file_t* vox_file_open(vox_mpool_t* mpool, const char* path, vox_file_mode_t mode) {
    if (!mpool || !path) return NULL;
    
    vox_file_t* file = (vox_file_t*)vox_mpool_alloc(mpool, sizeof(vox_file_t));
    if (!file) return NULL;
    
    memset(file, 0, sizeof(vox_file_t));
    file->mpool = mpool;
    file->mode = mode;
    
#ifdef VOX_OS_WINDOWS
    DWORD dwDesiredAccess = 0;
    DWORD dwCreationDisposition = 0;
    DWORD dwShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    
    switch (mode) {
        case VOX_FILE_MODE_READ:
            dwDesiredAccess = GENERIC_READ;
            dwCreationDisposition = OPEN_EXISTING;
            break;
        case VOX_FILE_MODE_WRITE:
            dwDesiredAccess = GENERIC_WRITE;
            dwCreationDisposition = CREATE_ALWAYS;
            break;
        case VOX_FILE_MODE_APPEND:
            dwDesiredAccess = FILE_APPEND_DATA | GENERIC_WRITE;
            dwCreationDisposition = OPEN_ALWAYS;
            break;
        case VOX_FILE_MODE_READ_WRITE:
            dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
            dwCreationDisposition = OPEN_ALWAYS;
            break;
        case VOX_FILE_MODE_READ_APPEND:
            dwDesiredAccess = GENERIC_READ | FILE_APPEND_DATA | GENERIC_WRITE;
            dwCreationDisposition = OPEN_ALWAYS;
            break;
    }
    
    /* 转换路径为宽字符 */
    int path_len = (int)strlen(path) + 1;
    wchar_t* wpath = (wchar_t*)vox_mpool_alloc(mpool, path_len * sizeof(wchar_t));
    if (!wpath) {
        vox_mpool_free(mpool, file);
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_len);
    
    file->handle = CreateFileW(wpath, dwDesiredAccess, dwShareMode, NULL,
                               dwCreationDisposition, FILE_ATTRIBUTE_NORMAL, NULL);
    
    vox_mpool_free(mpool, wpath);
    
    if (file->handle == INVALID_HANDLE_VALUE) {
        vox_mpool_free(mpool, file);
        return NULL;
    }
    
    /* 如果是追加模式，移动到文件末尾 */
    if (mode == VOX_FILE_MODE_APPEND || mode == VOX_FILE_MODE_READ_APPEND) {
        SetFilePointer(file->handle, 0, NULL, FILE_END);
    }
#else
    int flags = 0;
    mode_t file_mode = 0644;
    
    switch (mode) {
        case VOX_FILE_MODE_READ:
            flags = O_RDONLY;
            break;
        case VOX_FILE_MODE_WRITE:
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case VOX_FILE_MODE_APPEND:
            flags = O_WRONLY | O_CREAT | O_APPEND;
            break;
        case VOX_FILE_MODE_READ_WRITE:
            flags = O_RDWR | O_CREAT;
            break;
        case VOX_FILE_MODE_READ_APPEND:
            flags = O_RDWR | O_CREAT | O_APPEND;
            break;
    }
    
    file->fd = open(path, flags, file_mode);
    if (file->fd < 0) {
        vox_mpool_free(mpool, file);
        return NULL;
    }
#endif
    
    return file;
}

/* 关闭文件 */
int vox_file_close(vox_file_t* file) {
    if (!file) return -1;
    
    int ret = 0;
    
#ifdef VOX_OS_WINDOWS
    if (file->handle != INVALID_HANDLE_VALUE) {
        if (!CloseHandle(file->handle)) {
            ret = -1;
        }
    }
#else
    if (file->fd >= 0) {
        if (close(file->fd) != 0) {
            ret = -1;
        }
    }
#endif
    
    vox_mpool_t* mpool = file->mpool;
    vox_mpool_free(mpool, file);
    
    return ret;
}

intptr_t vox_file_get_fd(vox_file_t* file) {
    if (!file) return (intptr_t)-1;
#ifdef VOX_OS_WINDOWS
    if (file->handle == INVALID_HANDLE_VALUE) return (intptr_t)-1;
    return (intptr_t)file->handle;
#else
    if (file->fd < 0) return (intptr_t)-1;
    return (intptr_t)file->fd;
#endif
}

/* 读取文件数据 */
int64_t vox_file_read(vox_file_t* file, void* buffer, size_t size) {
    if (!file || !buffer || size == 0) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD bytes_read = 0;
    if (!ReadFile(file->handle, buffer, (DWORD)size, &bytes_read, NULL)) {
        return -1;
    }
    return (int64_t)bytes_read;
#else
    ssize_t bytes_read = read(file->fd, buffer, size);
    return (int64_t)bytes_read;
#endif
}

/* 写入文件数据 */
int64_t vox_file_write(vox_file_t* file, const void* buffer, size_t size) {
    if (!file || !buffer || size == 0) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD bytes_written = 0;
    if (!WriteFile(file->handle, buffer, (DWORD)size, &bytes_written, NULL)) {
        return -1;
    }
    return (int64_t)bytes_written;
#else
    ssize_t bytes_written = write(file->fd, buffer, size);
    return (int64_t)bytes_written;
#endif
}

/* 刷新文件缓冲区 */
int vox_file_flush(vox_file_t* file) {
    if (!file) return -1;
    
#ifdef VOX_OS_WINDOWS
    return FlushFileBuffers(file->handle) ? 0 : -1;
#else
    return fsync(file->fd) == 0 ? 0 : -1;
#endif
}

/* 文件定位 */
int64_t vox_file_seek(vox_file_t* file, int64_t offset, vox_file_seek_t whence) {
    if (!file) return -1;
    
#ifdef VOX_OS_WINDOWS
    DWORD dwMoveMethod;
    switch (whence) {
        case VOX_FILE_SEEK_SET:
            dwMoveMethod = FILE_BEGIN;
            break;
        case VOX_FILE_SEEK_CUR:
            dwMoveMethod = FILE_CURRENT;
            break;
        case VOX_FILE_SEEK_END:
            dwMoveMethod = FILE_END;
            break;
        default:
            return -1;
    }
    
    LARGE_INTEGER li;
    li.QuadPart = offset;
    li.LowPart = SetFilePointer(file->handle, li.LowPart, &li.HighPart, dwMoveMethod);
    if (li.LowPart == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
        return -1;
    }
    return li.QuadPart;
#else
    int posix_whence;
    switch (whence) {
        case VOX_FILE_SEEK_SET:
            posix_whence = SEEK_SET;
            break;
        case VOX_FILE_SEEK_CUR:
            posix_whence = SEEK_CUR;
            break;
        case VOX_FILE_SEEK_END:
            posix_whence = SEEK_END;
            break;
        default:
            return -1;
    }
    
    return lseek(file->fd, offset, posix_whence);
#endif
}

/* 获取当前文件位置 */
int64_t vox_file_tell(vox_file_t* file) {
    return vox_file_seek(file, 0, VOX_FILE_SEEK_CUR);
}

/* 获取文件大小 */
int64_t vox_file_size(vox_file_t* file) {
    if (!file) return -1;
    
    int64_t current_pos = vox_file_tell(file);
    if (current_pos < 0) return -1;
    
    int64_t size = vox_file_seek(file, 0, VOX_FILE_SEEK_END);
    if (size < 0) return -1;
    
    vox_file_seek(file, current_pos, VOX_FILE_SEEK_SET);
    
    return size;
}

/* 检查文件是否存在 */
bool vox_file_exists(const char* path) {
    if (!path) return false;
    
#ifdef VOX_OS_WINDOWS
    DWORD dwAttrib = GetFileAttributesA(path);
    return (dwAttrib != INVALID_FILE_ATTRIBUTES);
#else
    return access(path, F_OK) == 0;
#endif
}

/* 获取文件信息 */
int vox_file_stat(const char* path, vox_file_info_t* info) {
    if (!path) return -1;
    
    struct stat st;
    
#ifdef VOX_OS_WINDOWS
    if (stat(path, &st) != 0) {
        if (info) {
            memset(info, 0, sizeof(vox_file_info_t));
        }
        return -1;
    }
    
    if (info) {
        info->exists = true;
        info->is_directory = S_ISDIR(st.st_mode);
        info->is_regular_file = S_ISREG(st.st_mode);
        info->size = (int64_t)st.st_size;
        info->modified_time = (int64_t)st.st_mtime;
        info->accessed_time = (int64_t)st.st_atime;
        info->created_time = (int64_t)st.st_ctime;
    }
#else
    if (stat(path, &st) != 0) {
        if (info) {
            memset(info, 0, sizeof(vox_file_info_t));
        }
        return -1;
    }
    
    if (info) {
        info->exists = true;
        info->is_directory = S_ISDIR(st.st_mode);
        info->is_regular_file = S_ISREG(st.st_mode);
        info->size = (int64_t)st.st_size;
        info->modified_time = (int64_t)st.st_mtime;
        info->accessed_time = (int64_t)st.st_atime;
        info->created_time = (int64_t)st.st_ctime;
    }
#endif
    
    return 0;
}

/* 删除文件 */
int vox_file_remove(vox_mpool_t* mpool, const char* path) {
    if (!mpool || !path) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* 转换路径为宽字符 */
    int path_len = (int)strlen(path) + 1;
    wchar_t* wpath = (wchar_t*)vox_mpool_alloc(mpool, path_len * sizeof(wchar_t));
    if (!wpath) return -1;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_len);
    
    int ret = DeleteFileW(wpath) ? 0 : -1;
    vox_mpool_free(mpool, wpath);
    return ret;
#else
    return unlink(path) == 0 ? 0 : -1;
#endif
}

/* 重命名文件 */
int vox_file_rename(vox_mpool_t* mpool, const char* old_path, const char* new_path) {
    if (!mpool || !old_path || !new_path) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* 转换路径为宽字符 */
    int old_len = (int)strlen(old_path) + 1;
    int new_len = (int)strlen(new_path) + 1;
    wchar_t* wold = (wchar_t*)vox_mpool_alloc(mpool, old_len * sizeof(wchar_t));
    wchar_t* wnew = (wchar_t*)vox_mpool_alloc(mpool, new_len * sizeof(wchar_t));
    if (!wold || !wnew) return -1;
    MultiByteToWideChar(CP_UTF8, 0, old_path, -1, wold, old_len);
    MultiByteToWideChar(CP_UTF8, 0, new_path, -1, wnew, new_len);
    
    int ret = MoveFileW(wold, wnew) ? 0 : -1;
    vox_mpool_free(mpool, wold);
    vox_mpool_free(mpool, wnew);
    return ret;
#else
    return rename(old_path, new_path) == 0 ? 0 : -1;
#endif
}

/* 复制文件 */
int vox_file_copy(vox_mpool_t* mpool, const char* src_path, const char* dst_path) {
    if (!mpool || !src_path || !dst_path) return -1;
    
    /* 打开源文件 */
    vox_file_t* src = vox_file_open(mpool, src_path, VOX_FILE_MODE_READ);
    if (!src) {
        return -1;
    }
    
    /* 打开目标文件 */
    vox_file_t* dst = vox_file_open(mpool, dst_path, VOX_FILE_MODE_WRITE);
    if (!dst) {
        vox_file_close(src);
        return -1;
    }
    
    /* 复制数据 */
    char buffer[8192];
    int64_t bytes_read;
    int ret = 0;
    
    while ((bytes_read = vox_file_read(src, buffer, sizeof(buffer))) > 0) {
        int64_t bytes_written = vox_file_write(dst, buffer, (size_t)bytes_read);
        if (bytes_written != bytes_read) {
            ret = -1;
            break;
        }
    }
    
    if (bytes_read < 0) {
        ret = -1;
    }
    
    vox_file_close(src);
    vox_file_close(dst);
    
    return ret;
}

/* 创建目录 */
int vox_file_mkdir(vox_mpool_t* mpool, const char* path, bool recursive) {
    if (!mpool || !path) return -1;
    
    if (!recursive) {
#ifdef VOX_OS_WINDOWS
        /* 转换路径为宽字符 */
        int path_len = (int)strlen(path) + 1;
        wchar_t* wpath = (wchar_t*)vox_mpool_alloc(mpool, path_len * sizeof(wchar_t));
        if (!wpath) return -1;
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_len);
        
        int ret = CreateDirectoryW(wpath, NULL) ? 0 : -1;
        vox_mpool_free(mpool, wpath);
        return ret;
#else
        return mkdir(path, 0755) == 0 ? 0 : -1;
#endif
    }
    
    /* 递归创建 */
    size_t path_len = strlen(path) + 1;
    char* path_copy = (char*)vox_mpool_alloc(mpool, path_len);
    if (!path_copy) return -1;
    strcpy(path_copy, path);
    
    char* p = path_copy;
    char sep = vox_file_separator();
    
    /* 跳过开头的分隔符 */
    if (*p == sep) {
        p++;
    }
    
    int ret = 0;
    while (*p) {
        /* 找到下一个分隔符 */
        while (*p && *p != sep) {
            p++;
        }
        
        /* 临时截断路径 */
        char old_char = *p;
        *p = '\0';
        
        /* 创建目录（如果不存在） */
        if (!vox_file_exists(path_copy)) {
            if (vox_file_mkdir(mpool, path_copy, false) != 0) {
                ret = -1;
                break;
            }
        }
        
        /* 恢复字符 */
        if (old_char) {
            *p = old_char;
            p++;
        } else {
            break;
        }
    }
    
    vox_mpool_free(mpool, path_copy);
    return ret;
}

/* 递归删除目录的辅助回调函数 */
static int rmdir_walk_callback(const char* file_path, const vox_file_info_t* info, void* user_data) {
    vox_mpool_t* mpool = (vox_mpool_t*)user_data;
    if (!mpool) return -1;
    
    if (info->is_directory) {
        /* 递归删除子目录 */
        if (vox_file_rmdir(mpool, file_path, true) != 0) {
            return -1;  /* 停止遍历 */
        }
    } else {
        /* 删除文件 */
        if (vox_file_remove(mpool, file_path) != 0) {
            return -1;  /* 停止遍历 */
        }
    }
    return 0;
}

/* 删除目录 */
int vox_file_rmdir(vox_mpool_t* mpool, const char* path, bool recursive) {
    if (!mpool || !path) return -1;
    
    if (!recursive) {
#ifdef VOX_OS_WINDOWS
        /* 转换路径为宽字符 */
        int path_len = (int)strlen(path) + 1;
        wchar_t* wpath = (wchar_t*)vox_mpool_alloc(mpool, path_len * sizeof(wchar_t));
        if (!wpath) return -1;
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_len);
        
        int ret = RemoveDirectoryW(wpath) ? 0 : -1;
        vox_mpool_free(mpool, wpath);
        return ret;
#else
        return rmdir(path) == 0 ? 0 : -1;
#endif
    }
    
    /* 递归删除：先删除所有子项，再删除目录本身 */
    /* 遍历目录并删除所有内容 */
    int ret = vox_file_walk(mpool, path, rmdir_walk_callback, mpool);
    
    if (ret >= 0) {
        /* 删除目录本身 */
        ret = vox_file_rmdir(mpool, path, false);
    }
    
    return ret;
}

/* 遍历目录 */
int vox_file_walk(vox_mpool_t* mpool, const char* path, vox_file_walk_callback_t callback, void* user_data) {
    if (!mpool || !path || !callback) return -1;
    
    vox_file_info_t info;
    if (vox_file_stat(path, &info) != 0) {
        return -1;
    }
    
    if (!info.is_directory) {
        return -1;
    }
    
    int count = 0;
    
#ifdef VOX_OS_WINDOWS
    /* 构建搜索路径 */
    size_t path_len = strlen(path) + 4;
    char* search_path = (char*)vox_mpool_alloc(mpool, path_len);
    if (!search_path) return -1;
    sprintf(search_path, "%s\\*", path);
    
    /* 转换路径为宽字符 */
    int wpath_len = (int)strlen(search_path) + 1;
    wchar_t* wpath = (wchar_t*)vox_mpool_alloc(mpool, wpath_len * sizeof(wchar_t));
    if (!wpath) {
        vox_mpool_free(mpool, search_path);
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, search_path, -1, wpath, wpath_len);
    
    WIN32_FIND_DATAW find_data;
    HANDLE hFind = FindFirstFileW(wpath, &find_data);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        vox_mpool_free(mpool, wpath);
        vox_mpool_free(mpool, search_path);
        return -1;
    }
    
    do {
        /* 跳过 . 和 .. */
        if (wcscmp(find_data.cFileName, L".") == 0 || 
            wcscmp(find_data.cFileName, L"..") == 0) {
            continue;
        }
        
        /* 转换文件名回UTF-8 */
        int name_len = WideCharToMultiByte(CP_UTF8, 0, find_data.cFileName, -1, NULL, 0, NULL, NULL);
        char* name = (char*)vox_mpool_alloc(mpool, name_len);
        if (!name) break;
        WideCharToMultiByte(CP_UTF8, 0, find_data.cFileName, -1, name, name_len, NULL, NULL);
        
        /* 构建完整路径 */
        char* full_path = vox_file_join(mpool, path, name);
        if (!full_path) {
            vox_mpool_free(mpool, name);
            continue;
        }
        
        /* 获取文件信息 */
        vox_file_info_t file_info;
        if (vox_file_stat(full_path, &file_info) == 0) {
            file_info.is_directory = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            file_info.is_regular_file = !file_info.is_directory;
            file_info.size = ((int64_t)find_data.nFileSizeHigh << 32) | find_data.nFileSizeLow;
            
            /* 转换文件时间 */
            FILETIME* ft = &find_data.ftLastWriteTime;
            ULARGE_INTEGER uli;
            uli.LowPart = ft->dwLowDateTime;
            uli.HighPart = ft->dwHighDateTime;
            file_info.modified_time = (int64_t)((uli.QuadPart / 10000000) - 11644473600);
            
            if (callback(full_path, &file_info, user_data) != 0) {
                vox_mpool_free(mpool, name);
                break;
            }
            count++;
        }
        
        vox_mpool_free(mpool, name);
    } while (FindNextFileW(hFind, &find_data));
    
    FindClose(hFind);
    vox_mpool_free(mpool, wpath);
    vox_mpool_free(mpool, search_path);
#else
    DIR* dir = opendir(path);
    if (!dir) return -1;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过 . 和 .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* 构建完整路径 */
        char* full_path = vox_file_join(mpool, path, entry->d_name);
        if (!full_path) {
            continue;
        }
        
        /* 获取文件信息 */
        vox_file_info_t file_info;
        if (vox_file_stat(full_path, &file_info) == 0) {
            if (callback(full_path, &file_info, user_data) != 0) {
                break;
            }
            count++;
        }
    }
    
    closedir(dir);
#endif
    
    return count;
}

/* 读取整个文件到内存 */
void* vox_file_read_all(vox_mpool_t* mpool, const char* path, size_t* size) {
    if (!mpool || !path) return NULL;
    
    vox_file_t* file = vox_file_open(mpool, path, VOX_FILE_MODE_READ);
    if (!file) return NULL;
    
    int64_t file_size = vox_file_size(file);
    if (file_size < 0) {
        vox_file_close(file);
        return NULL;
    }
    
    void* data = vox_mpool_alloc(mpool, (size_t)file_size + 1);
    if (!data) {
        vox_file_close(file);
        return NULL;
    }
    
    int64_t bytes_read = vox_file_read(file, data, (size_t)file_size);
    vox_file_close(file);
    
    if (bytes_read != (int64_t)file_size) {
        vox_mpool_free(mpool, data);
        return NULL;
    }
    
    /* 添加字符串结束符（方便文本文件使用） */
    ((char*)data)[file_size] = '\0';
    
    if (size) {
        *size = (size_t)file_size;
    }
    
    return data;
}

/* 写入整个缓冲区到文件 */
int vox_file_write_all(vox_mpool_t* mpool, const char* path, const void* data, size_t size) {
    if (!mpool || !path || !data) return -1;
    
    vox_file_t* file = vox_file_open(mpool, path, VOX_FILE_MODE_WRITE);
    if (!file) {
        return -1;
    }
    
    int64_t bytes_written = vox_file_write(file, data, size);
    int ret = (bytes_written == (int64_t)size) ? 0 : -1;
    
    vox_file_close(file);
    
    return ret;
}

/* 获取当前工作目录 */
char* vox_file_getcwd(vox_mpool_t* mpool) {
    if (!mpool) return NULL;
    
#ifdef VOX_OS_WINDOWS
    DWORD len = GetCurrentDirectoryW(0, NULL);
    if (len == 0) return NULL;
    
    wchar_t* wpath = (wchar_t*)vox_mpool_alloc(mpool, len * sizeof(wchar_t));
    if (!wpath) return NULL;
    
    if (GetCurrentDirectoryW(len, wpath) == 0) {
        vox_mpool_free(mpool, wpath);
        return NULL;
    }
    
    /* 转换为UTF-8 */
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, NULL, 0, NULL, NULL);
    char* path = (char*)vox_mpool_alloc(mpool, utf8_len);
    if (!path) {
        vox_mpool_free(mpool, wpath);
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, utf8_len, NULL, NULL);
    vox_mpool_free(mpool, wpath);
    
    return path;
#else
    char cwd_buf[PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf)) == NULL) return NULL;

    size_t len = strlen(cwd_buf) + 1;
    char* path = (char*)vox_mpool_alloc(mpool, len);
    if (!path) return NULL;
    memcpy(path, cwd_buf, len);
    return path;
#endif
}

/* 更改当前工作目录 */
int vox_file_chdir(vox_mpool_t* mpool, const char* path) {
    if (!mpool || !path) return -1;
    
#ifdef VOX_OS_WINDOWS
    /* 转换路径为宽字符 */
    int path_len = (int)strlen(path) + 1;
    wchar_t* wpath = (wchar_t*)vox_mpool_alloc(mpool, path_len * sizeof(wchar_t));
    if (!wpath) return -1;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_len);
    
    int ret = SetCurrentDirectoryW(wpath) ? 0 : -1;
    vox_mpool_free(mpool, wpath);
    return ret;
#else
    return chdir(path) == 0 ? 0 : -1;
#endif
}

/* 获取路径分隔符 */
char vox_file_separator(void) {
#ifdef VOX_OS_WINDOWS
    return '\\';
#else
    return '/';
#endif
}

/* 连接路径 */
char* vox_file_join(vox_mpool_t* mpool, const char* path1, const char* path2) {
    if (!mpool || !path1 || !path2) return NULL;
    
    char sep = vox_file_separator();
    size_t len1 = strlen(path1);
    size_t len2 = strlen(path2);
    
    /* 计算所需长度 */
    size_t total_len = len1 + len2 + 2;  /* +2 for separator and null terminator */
    bool need_sep = true;
    
    /* 检查 path1 末尾是否有分隔符（支持 / 和 \） */
    if (len1 > 0 && (path1[len1 - 1] == '/' || path1[len1 - 1] == '\\')) {
        need_sep = false;
        total_len--;
    }
    /* 检查 path2 开头是否有分隔符（支持 / 和 \） */
    if (len2 > 0 && (path2[0] == '/' || path2[0] == '\\')) {
        need_sep = false;
        total_len--;
    }
    
    char* result = (char*)vox_mpool_alloc(mpool, total_len);
    if (!result) return NULL;
    
    strcpy(result, path1);
    if (need_sep) {
        result[len1] = sep;
        result[len1 + 1] = '\0';
    }
    strcat(result, path2);
    
    return result;
}

/* 规范化路径 */
char* vox_file_normalize(vox_mpool_t* mpool, const char* path) {
    if (!mpool || !path) return NULL;
    
    size_t len = strlen(path);
    char* result = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!result) return NULL;
    
    char sep = vox_file_separator();
    const char* src = path;
    char* dst = result;
    bool last_was_sep = false;
    
    while (*src) {
        /* 统一处理 / 和 \ */
        if (*src == '/' || *src == '\\') {
            if (!last_was_sep) {
                *dst++ = sep;
                last_was_sep = true;
            }
            src++;
        } else if (*src == '.' && (src[1] == '/' || src[1] == '\\' || src[1] == '\0')) {
            /* 跳过 . */
            src++;
            if (*src == '/' || *src == '\\') src++;
        } else if (*src == '.' && src[1] == '.' && (src[2] == '/' || src[2] == '\\' || src[2] == '\0')) {
            /* 处理 .. */
            if (dst > result && (dst[-1] == '/' || dst[-1] == '\\')) {
                dst--;  /* 移除前一个分隔符 */
                while (dst > result && dst[-1] != '/' && dst[-1] != '\\') {
                    dst--;  /* 回退到上一个目录 */
                }
            }
            src += 2;
            if (*src == '/' || *src == '\\') src++;
        } else {
            *dst++ = *src++;
            last_was_sep = false;
        }
    }
    
    *dst = '\0';
    
    /* 移除末尾的分隔符（除非是根路径） */
    if (dst > result + 1 && (dst[-1] == '/' || dst[-1] == '\\')) {
        dst[-1] = '\0';
    }
    
    return result;
}

/* 获取文件扩展名 */
const char* vox_file_ext(const char* path) {
    if (!path) return NULL;
    
    const char* dot = strrchr(path, '.');
    /* 查找最后一个路径分隔符（支持 / 和 \） */
    const char* sep1 = strrchr(path, '/');
    const char* sep2 = strrchr(path, '\\');
    const char* sep = sep1 > sep2 ? sep1 : sep2;
    
    if (dot && (!sep || dot > sep)) {
        return dot;
    }
    
    return NULL;
}

/* 获取文件名（不含路径） */
const char* vox_file_basename(const char* path) {
    if (!path) return NULL;
    
    /* 查找最后一个路径分隔符（支持 / 和 \） */
    const char* sep1 = strrchr(path, '/');
    const char* sep2 = strrchr(path, '\\');
    const char* sep = sep1 > sep2 ? sep1 : sep2;
    
    if (sep) {
        return sep + 1;
    }
    
    return path;
}

/* 获取目录名（不含文件名） */
char* vox_file_dirname(vox_mpool_t* mpool, const char* path) {
    if (!mpool || !path) return NULL;
    
    /* 查找最后一个路径分隔符（支持 / 和 \） */
    const char* sep1 = strrchr(path, '/');
    const char* sep2 = strrchr(path, '\\');
    const char* sep = sep1 > sep2 ? sep1 : sep2;
    
    if (!sep) {
        /* 没有分隔符，返回当前目录 */
        char* result = (char*)vox_mpool_alloc(mpool, 2);
        if (!result) return NULL;
        result[0] = '.';
        result[1] = '\0';
        return result;
    }
    
    size_t len = sep - path;
    if (len == 0) {
        len = 1;  /* 根路径 */
    }
    
    char* result = (char*)vox_mpool_alloc(mpool, len + 1);
    if (!result) return NULL;
    
    memcpy(result, path, len);
    result[len] = '\0';
    
    return result;
}
