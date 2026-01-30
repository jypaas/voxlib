/*
 * vox_mpool.h - 高性能内存池
 * 支持多种固定大小的内存块分配 (16/32/64/128/256/512/1024/2048/4096/8192)
 */

 #ifndef VOX_MPOOL_H
 #define VOX_MPOOL_H
 
 #include <stddef.h>
 #include <stdint.h>
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
/* 内存池不透明类型 */
typedef struct vox_mpool vox_mpool_t;

/* 内存池配置结构 */
typedef struct {
    int thread_safe;         /* 是否线程安全，0=非线程安全（默认），非0=线程安全 */
    size_t initial_block_count;  /* 每个块大小对应的初始块数量，0表示使用默认值64 */
} vox_mpool_config_t;

/**
 * 创建内存池
 * @return 成功返回内存池指针，失败返回NULL
 */
vox_mpool_t* vox_mpool_create(void);

/**
 * 使用配置创建内存池
 * @param config 配置结构体指针，如果为NULL则使用默认配置（非线程安全）
 * @return 成功返回内存池指针，失败返回NULL
 */
vox_mpool_t* vox_mpool_create_with_config(const vox_mpool_config_t* config);
 
 /**
  * 从内存池分配内存
  * @param pool 内存池指针
  * @param size 请求的内存大小（字节）
  * @return 成功返回内存块指针，失败返回NULL
  */
 void* vox_mpool_alloc(vox_mpool_t* pool, size_t size);
 
 /**
  * 释放内存回内存池（自动识别块大小）
  * @param pool 内存池指针
  * @param ptr 要释放的内存块指针
  */
 void vox_mpool_free(vox_mpool_t* pool, void* ptr);
 
 /**
  * 重新分配内存（自动识别原块大小）
  * @param pool 内存池指针
  * @param ptr 原内存块指针（NULL则相当于alloc）
  * @param new_size 新内存块大小（字节，0则相当于free）
  * @return 成功返回新内存块指针，失败返回NULL
  */
 void* vox_mpool_realloc(vox_mpool_t* pool, void* ptr, size_t new_size);
 
 /**
  * 获取已分配内存块的大小
  * @param pool 内存池指针
  * @param ptr 内存块指针
  * @return 返回块大小，失败返回0
  */
 size_t vox_mpool_get_size(vox_mpool_t* pool, void* ptr);
 
/**
 * 重置内存池（将所有块放回自由链表，重置统计信息）
 * 注意：此函数不会释放已分配但未释放的内存块，这些块将变为无效
 * @param pool 内存池指针
 */
void vox_mpool_reset(vox_mpool_t* pool);

/**
 * 销毁内存池并释放所有资源
 * @param pool 内存池指针
 */
void vox_mpool_destroy(vox_mpool_t* pool);

/**
 * 打印内存池统计信息
 * @param pool 内存池指针
 */
void vox_mpool_stats(vox_mpool_t* pool);
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif /* VOX_MPOOL_H */
