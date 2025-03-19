#pragma once
#include <stdint.h>

#define MFS_BAD_BLOCK_CONFIG_ERROR                      -1000
#define MFS_WRONG_MODE_ERROR                            -1001
#define MFS_FILE_NOT_FOUND_ERROR                        -1002
#define MFS_NO_SPACE_ERROR                              -1003
#define MFS_FILE_NAME_BAD_LEN_ERROR                     -1004
#define MFS_INTERNAL_ASSERTION_ERROR                    -1005
#define MFS_READBACK_ERROR                              -1006
#define MFS_BIRTHDAY_LIMIT_REACHED_ERROR                -1007

#define MFS_BIT_BUF_SIZE_BYTES(block_count) (((block_count) - 1) / 8 + 1)

typedef enum {
    MFS_MODE_READ,
    MFS_MODE_WRITE
} mfs_mode_t;

typedef struct {
    uint8_t * block_buf;
    uint8_t * bit_bufs[4];
    int block_size;
    int block_count;
    void * cb_ctx;
    int (*read_block)(void * cb_ctx, int block_index);
    int (*write_block)(void * cb_ctx, int block_index);
} mfs_conf_t;

typedef struct {
    const mfs_conf_t * conf;
    int file_count;
    uint32_t youngest;
    int open_file_mode;
    int open_file_block_cursor;
    int32_t open_file_match_index;
    uint32_t writer_checksum;
    int open_file_block;
    int open_file_first_block;
} mfs_t;

int mfs_mount(mfs_t * mfs, const mfs_conf_t * conf);
int mfs_file_count(mfs_t * mfs);
int mfs_list_files(mfs_t * mfs, void * list_file_cb_ctx, void (*list_file_cb)(void *, const char *));
int mfs_delete(mfs_t * mfs, const char * name);
int mfs_open(mfs_t * mfs, const char * name, mfs_mode_t mode);
int mfs_read(mfs_t * mfs, uint8_t * dst, int size);
int mfs_write(mfs_t * mfs, const uint8_t * src, int size);
int mfs_close(mfs_t * mfs);

