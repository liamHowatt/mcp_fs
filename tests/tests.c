#include "../mcp_fs.h"

#include <string.h>
#include <stdio.h>

#define BLOCK_SIZE 2048
#define BLOCK_COUNT 5

#define ASSERT(expr) do { if(!(expr)) {printf("%s:%d failed\n", __func__, __LINE__); return;} } while(0)

static uint8_t memory_blocks[BLOCK_SIZE * BLOCK_COUNT] = {0};

static int read_block(void * cb_ctx, int block_index, void * dst)
{
    memcpy(dst, memory_blocks + (block_index * BLOCK_SIZE), BLOCK_SIZE);
    return 0;
}

static int write_block(void * cb_ctx, int block_index, const void * src)
{
    memcpy(memory_blocks + (block_index * BLOCK_SIZE), src, BLOCK_SIZE);
    return 0;
}

typedef struct {
    const char * fname;
    bool found;
} list_file_ctx_entry_t;

typedef struct {
    bool duplicates_found;
    bool unexpected_found;
    list_file_ctx_entry_t entries[];
} list_file_ctx_t;

static void list_file_cb(void * list_file_cb_ctx, const char * fname)
{
    list_file_ctx_t * ctx = list_file_cb_ctx;

    list_file_ctx_entry_t * entry = ctx->entries;

    while(entry->fname) {
        if(0 == strcmp(fname, entry->fname)) {
            if(entry->found) {
                ctx->duplicates_found = true;
            }
            entry->found = true;
            return;
        }

        entry++;
    }

    ctx->unexpected_found = true;
}

static uint8_t aligned_aux_memory[MFS_ALIGNED_AUX_MEMORY_SIZE(BLOCK_SIZE, BLOCK_COUNT)] __attribute__((aligned));
static const mfs_conf_t conf = {
    .aligned_aux_memory = aligned_aux_memory,
    BLOCK_SIZE,
    BLOCK_COUNT,
    NULL,
    read_block,
    write_block
};
static mfs_t mfs;

static void test_1(void)
{
    int res;

    memset(memory_blocks, 0, sizeof(memory_blocks));
    res = mfs_mount(&mfs, &conf);
    ASSERT(res == 0);


    res = mfs_open(&mfs, "one", MFS_MODE_WRITE);
    ASSERT(res == 0);

    uint8_t some_buffer[3000];
    memset(some_buffer, 0x22, sizeof(some_buffer));

    res = mfs_write(&mfs, some_buffer, 2150);
    ASSERT(res == 2150);

    res = mfs_close(&mfs);
    ASSERT(res == 0);


    res = mfs_open(&mfs, "two", MFS_MODE_WRITE);
    ASSERT(res == 0);

    res = mfs_write(&mfs, some_buffer, 150);
    ASSERT(res == 150);

    res = mfs_close(&mfs);
    ASSERT(res == 0);


    res = mfs_file_count(&mfs);
    ASSERT(res == 2);


    res = mfs_open(&mfs, "one", MFS_MODE_WRITE);
    ASSERT(res == 0);

    res = mfs_write(&mfs, some_buffer, 2150);
    ASSERT(res == 2150);

    res = mfs_close(&mfs);
    ASSERT(res == 0);


    res = mfs_file_count(&mfs);
    ASSERT(res == 2);

    /* need static for flexible array member */
    static list_file_ctx_t list_files_ctx = {.entries={{"one"}, {"two"}, {NULL}}};
    res = mfs_list_files(&mfs, &list_files_ctx, list_file_cb);
    ASSERT(res == 0);
    ASSERT(!list_files_ctx.duplicates_found);
    ASSERT(!list_files_ctx.unexpected_found);
    ASSERT(list_files_ctx.entries[0].found);
    ASSERT(list_files_ctx.entries[1].found);


    res = mfs_mount(&mfs, &conf);
    ASSERT(res == 0);


    res = mfs_open(&mfs, "two", MFS_MODE_WRITE);
    ASSERT(res == 0);

    res = mfs_write(&mfs, some_buffer, 2150);
    ASSERT(res == 2150);

    res = mfs_close(&mfs);
    ASSERT(res == 0);


    res = mfs_file_count(&mfs);
    ASSERT(res == 2);


    static list_file_ctx_t list_files_ctx2 = {.entries={{"one"}, {"two"}, {NULL}}};
    res = mfs_list_files(&mfs, &list_files_ctx2, list_file_cb);
    ASSERT(res == 0);
    ASSERT(!list_files_ctx2.duplicates_found);
    ASSERT(!list_files_ctx2.unexpected_found);
    ASSERT(list_files_ctx2.entries[0].found);
    ASSERT(list_files_ctx2.entries[1].found);
}

static void test_2(void)
{
    int res;

    memset(memory_blocks, 0, sizeof(memory_blocks));
    res = mfs_mount(&mfs, &conf);
    ASSERT(res == 0);


    res = mfs_open(&mfs, "one", MFS_MODE_WRITE);
    ASSERT(res == 0);

    uint8_t some_buffer[3000] = {0};

    res = mfs_write(&mfs, some_buffer, 2150);
    ASSERT(res == 2150);

    res = mfs_close(&mfs);
    ASSERT(res == 0);


    res = mfs_delete(&mfs, "one");
    ASSERT(res == 0);
}

int main()
{
    test_1();
    test_2();
}
