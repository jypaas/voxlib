/* ============================================================
 * test_queue.c - vox_queue 模块测试
 * ============================================================ */

#include "test_runner.h"
#include "../vox_queue.h"
#include "../vox_thread.h"
#include "../vox_atomic.h"

/* 测试创建和销毁 */
static void test_queue_create_destroy(vox_mpool_t* mpool) {
    vox_queue_t* queue = vox_queue_create(mpool);
    TEST_ASSERT_NOT_NULL(queue, "创建queue失败");
    TEST_ASSERT_EQ(vox_queue_size(queue), 0, "新queue大小应为0");
    TEST_ASSERT_EQ(vox_queue_empty(queue), 1, "新queue应为空");
    vox_queue_destroy(queue);
}

/* 测试enqueue和dequeue */
static void test_queue_enqueue_dequeue(vox_mpool_t* mpool) {
    vox_queue_t* queue = vox_queue_create(mpool);
    TEST_ASSERT_NOT_NULL(queue, "创建queue失败");
    
    int values[] = {1, 2, 3, 4, 5};
    
    /* 测试enqueue */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQ(vox_queue_enqueue(queue, &values[i]), 0, "enqueue失败");
        TEST_ASSERT_EQ(vox_queue_size(queue), (size_t)(i + 1), "queue大小不正确");
    }
    
    /* 测试peek */
    int* peek_val = (int*)vox_queue_peek(queue);
    TEST_ASSERT_NOT_NULL(peek_val, "peek失败");
    TEST_ASSERT_EQ(*peek_val, 1, "peek的值不正确");
    
    /* 测试dequeue */
    for (int i = 0; i < 5; i++) {
        int* val = (int*)vox_queue_dequeue(queue);
        TEST_ASSERT_NOT_NULL(val, "dequeue失败");
        TEST_ASSERT_EQ(*val, values[i], "dequeue的值不正确");
        TEST_ASSERT_EQ(vox_queue_size(queue), (size_t)(4 - i), "dequeue后大小不正确");
    }
    
    TEST_ASSERT_EQ(vox_queue_empty(queue), 1, "queue应为空");
    
    vox_queue_destroy(queue);
}

/* 测试FIFO特性 */
static void test_queue_fifo(vox_mpool_t* mpool) {
    vox_queue_t* queue = vox_queue_create(mpool);
    TEST_ASSERT_NOT_NULL(queue, "创建queue失败");
    
    int val1 = 10, val2 = 20, val3 = 30;
    
    vox_queue_enqueue(queue, &val1);
    vox_queue_enqueue(queue, &val2);
    vox_queue_enqueue(queue, &val3);
    
    /* 应该按FIFO顺序出队 */
    int* v1 = (int*)vox_queue_dequeue(queue);
    int* v2 = (int*)vox_queue_dequeue(queue);
    int* v3 = (int*)vox_queue_dequeue(queue);
    
    TEST_ASSERT_EQ(*v1, 10, "FIFO顺序错误");
    TEST_ASSERT_EQ(*v2, 20, "FIFO顺序错误");
    TEST_ASSERT_EQ(*v3, 30, "FIFO顺序错误");
    
    vox_queue_destroy(queue);
}

/* 测试clear */
static void test_queue_clear(vox_mpool_t* mpool) {
    vox_queue_t* queue = vox_queue_create(mpool);
    TEST_ASSERT_NOT_NULL(queue, "创建queue失败");
    
    int values[] = {1, 2, 3};
    for (int i = 0; i < 3; i++) {
        vox_queue_enqueue(queue, &values[i]);
    }
    
    vox_queue_clear(queue);
    TEST_ASSERT_EQ(vox_queue_size(queue), 0, "clear后大小应为0");
    TEST_ASSERT_EQ(vox_queue_empty(queue), 1, "clear后应为空");
    
    vox_queue_destroy(queue);
}

/* 测试空队列操作 */
static void test_queue_empty_ops(vox_mpool_t* mpool) {
    vox_queue_t* queue = vox_queue_create(mpool);
    TEST_ASSERT_NOT_NULL(queue, "创建queue失败");
    
    /* 从空队列dequeue应该返回NULL */
    void* val = vox_queue_dequeue(queue);
    TEST_ASSERT_NULL(val, "从空队列dequeue应返回NULL");
    
    /* 从空队列peek应该返回NULL */
    val = vox_queue_peek(queue);
    TEST_ASSERT_NULL(val, "从空队列peek应返回NULL");
    
    vox_queue_destroy(queue);
}

/* SPSC测试数据结构 */
typedef struct {
    vox_queue_t* queue;
    int* data_array;
    int total_items;
    int produced_count;
    int consumed_count;
} spsc_test_data_t;

/* SPSC生产者线程函数 */
static int spsc_producer_func(void* user_data) {
    spsc_test_data_t* data = (spsc_test_data_t*)user_data;
    
    for (int i = 0; i < data->total_items; i++) {
        while (vox_queue_enqueue(data->queue, &data->data_array[i]) != 0) {
            /* 队列满，稍等再试 */
            vox_thread_yield();
        }
        data->produced_count++;
    }
    
    return 0;
}

/* SPSC消费者线程函数 */
static int spsc_consumer_func(void* user_data) {
    spsc_test_data_t* data = (spsc_test_data_t*)user_data;
    
    while (data->consumed_count < data->total_items) {
        void* elem = vox_queue_dequeue(data->queue);
        if (elem) {
            data->consumed_count++;
        } else {
            /* 队列空，稍等再试 */
            vox_thread_yield();
        }
    }
    
    return 0;
}

/* 测试SPSC（单生产者单消费者）无锁队列 */
static void test_queue_spsc(vox_mpool_t* mpool) {
    vox_queue_config_t config = {0};
    config.type = VOX_QUEUE_TYPE_SPSC;
    config.initial_capacity = 1024;  /* 固定容量 */
    
    vox_queue_t* queue = vox_queue_create_with_config(mpool, &config);
    TEST_ASSERT_NOT_NULL(queue, "创建SPSC队列失败");
    TEST_ASSERT_EQ(vox_queue_capacity(queue), 1024, "SPSC队列容量不正确");
    
    /* 准备测试数据 */
    int data_array[1000];
    for (int i = 0; i < 1000; i++) {
        data_array[i] = i;
    }
    
    spsc_test_data_t data = {
        .queue = queue,
        .data_array = data_array,
        .total_items = 1000,
        .produced_count = 0,
        .consumed_count = 0
    };
    
    /* 创建生产者和消费者线程 */
    vox_thread_t* producer = vox_thread_create(mpool, spsc_producer_func, &data);
    vox_thread_t* consumer = vox_thread_create(mpool, spsc_consumer_func, &data);
    
    TEST_ASSERT_NOT_NULL(producer, "创建生产者线程失败");
    TEST_ASSERT_NOT_NULL(consumer, "创建消费者线程失败");
    
    /* 等待线程完成 */
    vox_thread_join(producer, NULL);
    vox_thread_join(consumer, NULL);
    
    /* 验证所有数据都被生产和消费 */
    TEST_ASSERT_EQ(data.produced_count, 1000, "SPSC生产者计数不正确");
    TEST_ASSERT_EQ(data.consumed_count, 1000, "SPSC消费者计数不正确");
    TEST_ASSERT_EQ(vox_queue_empty(queue), 1, "SPSC队列最终应为空");
    
    vox_queue_destroy(queue);
}

/* MPSC生产者线程数据 */
typedef struct {
    vox_queue_t* queue;
    int* data_array;
    int items_per_producer;
    int producer_id;
    int* produced_counts;
} mpsc_producer_data_t;

/* MPSC测试数据结构 */
typedef struct {
    vox_queue_t* queue;
    vox_atomic_int_t* consumed_count;  /* 使用原子计数器 */
    int total_items;
} mpsc_consumer_data_t;

/* MPSC生产者线程函数 */
static int mpsc_producer_func(void* user_data) {
    mpsc_producer_data_t* data = (mpsc_producer_data_t*)user_data;
    
    int start_idx = data->producer_id * data->items_per_producer;
    for (int i = 0; i < data->items_per_producer; i++) {
        int retry_count = 0;
        const int max_retries = 1000;  /* 最大重试次数 */
        
        while (vox_queue_enqueue(data->queue, &data->data_array[start_idx + i]) != 0) {
            /* 队列满，稍等再试 */
            retry_count++;
            if (retry_count >= max_retries) {
                /* 重试次数过多，使用更长的延迟 */
                vox_thread_sleep(1);  /* 休眠1毫秒 */
                retry_count = 0;
            } else {
                vox_thread_yield();
            }
        }
        data->produced_counts[data->producer_id]++;
    }
    
    return 0;
}

/* MPSC消费者线程函数 */
static int mpsc_consumer_func(void* user_data) {
    mpsc_consumer_data_t* data = (mpsc_consumer_data_t*)user_data;
    
    while (vox_atomic_int_load(data->consumed_count) < data->total_items) {
        void* elem = vox_queue_dequeue(data->queue);
        if (elem) {
            vox_atomic_int_increment(data->consumed_count);
        } else {
            /* 队列空，稍等再试 */
            vox_thread_sleep(1);  /* 休眠1毫秒，避免过度CPU占用 */
        }
    }
    
    return 0;
}

/* 测试MPSC（多生产者单消费者）无锁队列 */
static void test_queue_mpsc(vox_mpool_t* mpool) {
    vox_queue_config_t config = {0};
    config.type = VOX_QUEUE_TYPE_MPSC;
    config.initial_capacity = 2048;  /* 固定容量 */
    
    vox_queue_t* queue = vox_queue_create_with_config(mpool, &config);
    TEST_ASSERT_NOT_NULL(queue, "创建MPSC队列失败");
    TEST_ASSERT_EQ(vox_queue_capacity(queue), 2048, "MPSC队列容量不正确");
    
    /* 准备测试数据 */
    int producer_count = 5;
    int items_per_producer = 500;
    int total_items = producer_count * items_per_producer;
    
    int* data_array = (int*)vox_mpool_alloc(mpool, total_items * sizeof(int));
    int* produced_counts = (int*)vox_mpool_alloc(mpool, producer_count * sizeof(int));
    
    for (int i = 0; i < total_items; i++) {
        data_array[i] = i;
    }
    for (int i = 0; i < producer_count; i++) {
        produced_counts[i] = 0;
    }
    
    /* 创建生产者线程数据 */
    mpsc_producer_data_t* producer_data = (mpsc_producer_data_t*)vox_mpool_alloc(mpool, 
        producer_count * sizeof(mpsc_producer_data_t));
    
    /* 创建原子计数器用于消费者计数 */
    vox_atomic_int_t* consumed_count = vox_atomic_int_create(mpool, 0);
    TEST_ASSERT_NOT_NULL(consumed_count, "创建原子计数器失败");
    
    mpsc_consumer_data_t consumer_data = {
        .queue = queue,
        .consumed_count = consumed_count,
        .total_items = total_items
    };
    
    /* 创建多个生产者线程和一个消费者线程 */
    vox_thread_t* threads[6];  /* 5个生产者 + 1个消费者 */
    
    for (int i = 0; i < producer_count; i++) {
        producer_data[i].queue = queue;
        producer_data[i].data_array = data_array;
        producer_data[i].items_per_producer = items_per_producer;
        producer_data[i].producer_id = i;
        producer_data[i].produced_counts = produced_counts;
        threads[i] = vox_thread_create(mpool, mpsc_producer_func, &producer_data[i]);
        TEST_ASSERT_NOT_NULL(threads[i], "创建生产者线程失败");
    }
    
    threads[producer_count] = vox_thread_create(mpool, mpsc_consumer_func, &consumer_data);
    TEST_ASSERT_NOT_NULL(threads[producer_count], "创建消费者线程失败");
    
    /* 等待所有线程完成 */
    for (int i = 0; i <= producer_count; i++) {
        vox_thread_join(threads[i], NULL);
    }
    
    /* 验证所有数据都被生产和消费 */
    int total_produced = 0;
    for (int i = 0; i < producer_count; i++) {
        total_produced += produced_counts[i];
    }
    
    TEST_ASSERT_EQ(total_produced, total_items, "MPSC生产者总计数不正确");
    int32_t final_consumed = vox_atomic_int_load(consumed_count);
    TEST_ASSERT_EQ(final_consumed, total_items, "MPSC消费者计数不正确");
    TEST_ASSERT_EQ(vox_queue_empty(queue), 1, "MPSC队列最终应为空");
    
    vox_atomic_int_destroy(consumed_count);
    vox_queue_destroy(queue);
}

/* 测试队列容量和满状态 */
static void test_queue_capacity_full(vox_mpool_t* mpool) {
    vox_queue_config_t config = {0};
    config.type = VOX_QUEUE_TYPE_SPSC;
    config.initial_capacity = 16;  /* 小容量用于测试 */
    
    vox_queue_t* queue = vox_queue_create_with_config(mpool, &config);
    TEST_ASSERT_NOT_NULL(queue, "创建队列失败");
    TEST_ASSERT_EQ(vox_queue_capacity(queue), 16, "队列容量不正确");
    TEST_ASSERT_EQ(vox_queue_full(queue), 0, "新队列不应满");
    
    /* 填满队列 */
    int values[16];
    for (int i = 0; i < 15; i++) {  /* SPSC队列实际可用容量是capacity-1 */
        values[i] = i;
        TEST_ASSERT_EQ(vox_queue_enqueue(queue, &values[i]), 0, "入队失败");
    }
    
    /* 尝试入队应该失败（队列满） */
    int extra = 999;
    TEST_ASSERT_EQ(vox_queue_enqueue(queue, &extra), -1, "队列满时应返回-1");
    TEST_ASSERT_EQ(vox_queue_full(queue), 1, "队列应已满");
    
    vox_queue_destroy(queue);
}

/* 测试套件 */
test_case_t test_queue_cases[] = {
    {"create_destroy", test_queue_create_destroy},
    {"enqueue_dequeue", test_queue_enqueue_dequeue},
    {"fifo", test_queue_fifo},
    {"clear", test_queue_clear},
    {"empty_ops", test_queue_empty_ops},
    {"spsc", test_queue_spsc},
    {"mpsc", test_queue_mpsc},
    {"capacity_full", test_queue_capacity_full},
};

test_suite_t test_queue_suite = {
    "vox_queue",
    test_queue_cases,
    sizeof(test_queue_cases) / sizeof(test_queue_cases[0])
};
