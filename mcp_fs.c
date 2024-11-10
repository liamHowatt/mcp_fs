#include "mcp_fs.h"
#include <string.h>
#include <stdbool.h>
#include <limits.h>

/*

Block Layout


First block of file

birthday : u32
prefer_if_older : i32
file name : null-terminated string
data : u8[]
unoccupied data bytes : i32
next block idx or checksum : u32


Following blocks

data : u8[]
unoccupied data bytes : i32
next block idx or checksum : u32

*/

#define CHECKSUM_INIT_VAL 2166136261u

enum {
    FILE_START_BLOCKS,
    OCCUPIED_BLOCKS,
    SCRATCH_1,
    SCRATCH_2
};

static void set_bit(uint8_t * bit_buf, int bit_index)
{
    bit_buf[bit_index / 8] |= 1 << (bit_index % 8);
}

static bool get_bit(const uint8_t * bit_buf, int bit_index)
{
    return bit_buf[bit_index / 8] & (1 << (bit_index % 8));
}

static void clear_bit(uint8_t * bit_buf, int bit_index)
{
    bit_buf[bit_index / 8] &= ~(1 << (bit_index % 8));
}

static uint32_t checksum_update(uint32_t hash, const uint8_t * data, int len)
{
    for(int i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static int scan_file(mfs_t * mfs, int * end_index_dst, int block_index, uint8_t * scratch_bit_buf)
{
    int res;
    const mfs_conf_t * conf = mfs->conf;

    memset(scratch_bit_buf, 0, MFS_BIT_BUF_SIZE_BYTES(conf->block_count));
    uint32_t running_checksum = CHECKSUM_INIT_VAL;

    int current_block_index = block_index;
    while(1) {
        res = conf->read_block(conf->cb_ctx, current_block_index);
        if(res) return res;
        set_bit(scratch_bit_buf, current_block_index);
        *end_index_dst = current_block_index;

        int32_t unoccupied_data_bytes;
        memcpy(&unoccupied_data_bytes, conf->block_buf + (conf->block_size - 8), 4);
        bool has_next_block = unoccupied_data_bytes < 0;
        uint32_t next_block_or_target_checksum;
        memcpy(&next_block_or_target_checksum, conf->block_buf + (conf->block_size - 4), 4);
        if(!has_next_block) {
            running_checksum = checksum_update(running_checksum, conf->block_buf, conf->block_size - 4);
            if(running_checksum != next_block_or_target_checksum) {
                *end_index_dst = -1;
            }
            return 0;
        }
        if(next_block_or_target_checksum >= conf->block_count
            || get_bit(conf->bit_bufs[OCCUPIED_BLOCKS], next_block_or_target_checksum)
            || get_bit(scratch_bit_buf, next_block_or_target_checksum)) {
            *end_index_dst = -1;
            return 0;
        }
        running_checksum = checksum_update(running_checksum, conf->block_buf, conf->block_size);
        current_block_index = next_block_or_target_checksum;
    }
}

static int mount_inner(mfs_t * mfs, int file_initial_idx)
{
    int res;
    const mfs_conf_t * conf = mfs->conf;

    int file_end_idx_this;
    res = scan_file(mfs, &file_end_idx_this, file_initial_idx, conf->bit_bufs[SCRATCH_1]);
    if(res) return res;
    if(file_end_idx_this < 0) {
        return 0;
    }

    res = conf->read_block(conf->cb_ctx, file_initial_idx);
    if(res) return res;

    uint32_t birthday_this;
    memcpy(&birthday_this, conf->block_buf, 4);

    int32_t preferred_if_older;
    memcpy(&preferred_if_older, conf->block_buf + 4, 4);
    if(preferred_if_older < 0) {
        goto label_end_success;
    }

    int file_end_idx_other;
    res = scan_file(mfs, &file_end_idx_other, preferred_if_older, conf->bit_bufs[SCRATCH_2]);
    if(res) return res;
    if(file_end_idx_other < 0) {
        goto label_end_success;
    }

    res = conf->read_block(conf->cb_ctx, preferred_if_older);
    if(res) return res;

    uint32_t birthday_other;
    memcpy(&birthday_other, conf->block_buf, 4);
    if(birthday_other <= birthday_this) {
        return 0;
    }

label_end_success:
    if(birthday_this > mfs->youngest) mfs->youngest = birthday_this;
    set_bit(conf->bit_bufs[FILE_START_BLOCKS], file_initial_idx);
    mfs->file_count += 1;
    int bit_buf_len = MFS_BIT_BUF_SIZE_BYTES(conf->block_count);
    for(int i = 0; i < bit_buf_len; i++) {
        conf->bit_bufs[OCCUPIED_BLOCKS][i] |= conf->bit_bufs[SCRATCH_1][i];
    }
    return 0;
}

int mfs_mount(mfs_t * mfs, const mfs_conf_t * conf)
{
    int res;

    if(conf->block_size < (4 + 4 + 1 + 1 + 4 + 4)
       || conf->block_count < 1) {
        return MFS_BAD_BLOCK_CONFIG;
    }

    mfs->conf = conf;
    mfs->file_count = 0;
    mfs->youngest = 0;
    mfs->open_file_mode = -1;

    memset(conf->bit_bufs[FILE_START_BLOCKS], 0, MFS_BIT_BUF_SIZE_BYTES(conf->block_count));
    memset(conf->bit_bufs[OCCUPIED_BLOCKS], 0, MFS_BIT_BUF_SIZE_BYTES(conf->block_count));

    for(int file_initial_idx = 0; file_initial_idx < conf->block_count; file_initial_idx++) {
        res = mount_inner(mfs, file_initial_idx);
        if(res) return res;
    }

    return 0;
}

int mfs_file_count(mfs_t * mfs)
{
    if(mfs->open_file_mode != -1) {
        return MFS_WRONG_MODE_ERROR;
    }

    return mfs->file_count;
}

int mfs_list_files(mfs_t * mfs, void * list_file_cb_ctx, void (*list_file_cb)(void *, const char *))
{
    if(mfs->open_file_mode != -1) {
        return MFS_WRONG_MODE_ERROR;
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;

    int files_left = mfs->file_count;
    for(int i = 0; files_left; i++) {
        if(!get_bit(conf->bit_bufs[FILE_START_BLOCKS], i)) {
            continue;
        }
        res = conf->read_block(conf->cb_ctx, i);
        if(res) return res;
        list_file_cb(list_file_cb_ctx, conf->block_buf + 8);
        files_left--;
    }

    return 0;
}

int mfs_delete(mfs_t * mfs, const char * name)
{
    if(mfs->open_file_mode != -1) {
        return MFS_WRONG_MODE_ERROR;
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;
    int i;

    int name_len = strlen(name);
    if(name_len > conf->block_size - (4 + 4 + 1 + 4 + 4)
       || name_len < 1) {
        return MFS_FILE_NAME_BAD_LEN_ERROR;
    }

    int files_left = mfs->file_count;
    for(i = 0; files_left; i++) {
        if(!get_bit(conf->bit_bufs[FILE_START_BLOCKS], i)) {
            continue;
        }
        res = conf->read_block(conf->cb_ctx, i);
        if(res) return res;

        if(0 == strcmp(name, conf->block_buf + 8)) {
            break;
        }

        files_left--;
    }

    if(!files_left) {
        return MFS_FILE_NOT_FOUND_ERROR;
    }
    int delete_file_page_1 = i;

    clear_bit(conf->bit_bufs[FILE_START_BLOCKS], delete_file_page_1);

    res = conf->read_block(conf->cb_ctx, delete_file_page_1);
    if(res) return res;
    uint32_t birthday;
    memcpy(&birthday, conf->block_buf, 4);
    if(birthday == mfs->youngest) mfs->youngest--;

    int end_idx;
    res = scan_file(mfs, &end_idx, delete_file_page_1, conf->bit_bufs[SCRATCH_1]);
    if(res) return res;
    if(end_idx < 0) return MFS_INTERNAL_ASSERTION_ERROR;

    int bit_buf_len = MFS_BIT_BUF_SIZE_BYTES(conf->block_count);
    for(i = 0; i < bit_buf_len; i++) {
        conf->bit_bufs[OCCUPIED_BLOCKS][i] &= ~conf->bit_bufs[SCRATCH_1][i];
    }

    /* clobber the first page */
    memset(conf->block_buf, 0xff, conf->block_size);
    res = conf->write_block(conf->cb_ctx, delete_file_page_1);
    if(res) return res;
    res = conf->read_block(conf->cb_ctx, delete_file_page_1);
    if(res) return res;
    for(i = 0; i < conf->block_size; i++) {
        if(conf->block_buf[i] != 0xff) return MFS_READBACK_ERROR;
    }

    mfs->file_count -= 1;

    return 0;
}

int mfs_open(mfs_t * mfs, const char * name, mfs_mode_t mode)
{
    if(mfs->open_file_mode != -1) {
        return MFS_WRONG_MODE_ERROR;
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;
    int i;

    int name_len = strlen(name);
    if(name_len > conf->block_size - (4 + 4 + 1 + 1 + 4 + 4)
       || name_len < 1) {
        return MFS_FILE_NAME_BAD_LEN_ERROR;
    }

    int files_left = mfs->file_count;
    for(i = 0; files_left; i++) {
        if(!get_bit(conf->bit_bufs[FILE_START_BLOCKS], i)) {
            continue;
        }
        res = conf->read_block(conf->cb_ctx, i);
        if(res) return res;

        if(0 == strcmp(name, conf->block_buf + 8)) {
            break;
        }

        files_left--;
    }

    if(mode == MFS_MODE_READ) {
        if(!files_left) {
            return MFS_FILE_NOT_FOUND_ERROR;
        }
    }
    else {
        mfs->open_file_match_index = files_left ? i : -1;
        for(i = 0; i < conf->block_count; i++) {
            if(!get_bit(conf->bit_bufs[OCCUPIED_BLOCKS], i)) break;
        }
        if(i == conf->block_count) {
            return MFS_NO_SPACE_ERROR;
        }
        set_bit(conf->bit_bufs[OCCUPIED_BLOCKS], i);
        set_bit(conf->bit_bufs[FILE_START_BLOCKS], i);
        if(mfs->youngest == UINT32_MAX) {
            return MFS_BIRTHDAY_LIMIT_REACHED_ERROR;
        }
        mfs->youngest += 1;
        memcpy(conf->block_buf, &mfs->youngest, 4);
        memcpy(conf->block_buf + 4, &mfs->open_file_match_index, 4);
        strcpy(conf->block_buf + 8, name);
        mfs->writer_checksum = checksum_update(CHECKSUM_INIT_VAL, conf->block_buf, 8 + name_len + 1);
        mfs->open_file_block = i;
        mfs->open_file_first_block = i;
    }

    mfs->open_file_block_cursor = 8 + name_len + 1;

    mfs->open_file_mode = mode;
    return 0;
}

int mfs_read(mfs_t * mfs, uint8_t * dst, int size)
{
    if(mfs->open_file_mode != MFS_MODE_READ) {
        return MFS_WRONG_MODE_ERROR;
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;

    int total_read = 0;

    while(size) {
        int32_t unoccupied_data_bytes;
        memcpy(&unoccupied_data_bytes, conf->block_buf + (conf->block_size - 8), 4);
        bool has_next_block = unoccupied_data_bytes < 0;
        if(has_next_block) unoccupied_data_bytes = 0;

        int block_len_remaining = conf->block_size - mfs->open_file_block_cursor - unoccupied_data_bytes - 8;

        if(!block_len_remaining) {
            if(!has_next_block) {
                break;
            }

            uint32_t new_block_idx;
            memcpy(&new_block_idx, conf->block_buf + (conf->block_size - 4), 4);

            res = conf->read_block(conf->cb_ctx, new_block_idx);
            if(res) return res;

            mfs->open_file_block_cursor = 0;

            memcpy(&unoccupied_data_bytes, conf->block_buf + (conf->block_size - 8), 4);
            if(unoccupied_data_bytes < 0) unoccupied_data_bytes = 0;
            block_len_remaining = conf->block_size  - unoccupied_data_bytes - 8;
        }

        int copy_amount = block_len_remaining < size ? block_len_remaining : size;

        memcpy(dst, &conf->block_buf[mfs->open_file_block_cursor], copy_amount);

        size -= copy_amount;
        dst += copy_amount;
        mfs->open_file_block_cursor += copy_amount;
        total_read += copy_amount;
    }

    return total_read;
}

int mfs_write(mfs_t * mfs, uint8_t * src, int size)
{
    if(mfs->open_file_mode != MFS_MODE_WRITE) {
        return MFS_WRONG_MODE_ERROR;
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;

    int write_size_left = size;

    while(write_size_left) {
        int block_len_remaining = conf->block_size - mfs->open_file_block_cursor - 8;

        if(!block_len_remaining) {
            uint32_t i;
            for(i = 0; i < conf->block_count; i++) {
                if(!get_bit(conf->bit_bufs[OCCUPIED_BLOCKS], i)) break;
            }
            if(i == conf->block_count) {
                return MFS_NO_SPACE_ERROR;
            }
            set_bit(conf->bit_bufs[OCCUPIED_BLOCKS], i);

            int32_t unoccupied_data_bytes = -1;
            memcpy(conf->block_buf + (conf->block_size - 8), &unoccupied_data_bytes, 4);
            memcpy(conf->block_buf + (conf->block_size - 4), &i, 4);
            mfs->writer_checksum = checksum_update(mfs->writer_checksum, conf->block_buf + (conf->block_size - 8), 8);

            res = conf->write_block(conf->cb_ctx, mfs->open_file_block);
            if(res) return res;

            mfs->open_file_block_cursor = 0;
            mfs->open_file_block = i;
            block_len_remaining = conf->block_size - 8;
        }

        int copy_amount = block_len_remaining < write_size_left ? block_len_remaining : write_size_left;

        mfs->writer_checksum = checksum_update(mfs->writer_checksum, src, copy_amount);
        memcpy(conf->block_buf + mfs->open_file_block_cursor, src, copy_amount);

        write_size_left -= copy_amount;
        src += copy_amount;
        mfs->open_file_block_cursor += copy_amount;
    }

    return size;
}

int mfs_close(mfs_t * mfs, uint8_t * src, int size)
{
    if(mfs->open_file_mode == -1) {
        return MFS_WRONG_MODE_ERROR;
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;

    if(mfs->open_file_mode == MFS_MODE_WRITE) {
        int32_t unoccupied_data_bytes = conf->block_size - mfs->open_file_block_cursor - 8;
        memset(conf->block_buf + mfs->open_file_block_cursor, 0xff, unoccupied_data_bytes);
        memcpy(conf->block_buf + (conf->block_size - 8), &unoccupied_data_bytes, 4);
        mfs->writer_checksum = checksum_update(mfs->writer_checksum, conf->block_buf + mfs->open_file_block_cursor, unoccupied_data_bytes + 4);
        memcpy(conf->block_buf + (conf->block_size - 4), &mfs->writer_checksum, 4);

        res = conf->write_block(conf->cb_ctx, mfs->open_file_block);
        if(res) return res;

        int end_index;
        res = scan_file(mfs, &end_index, mfs->open_file_first_block, conf->bit_bufs[SCRATCH_1]);
        if(res < 0) return MFS_READBACK_ERROR;

        if(mfs->open_file_match_index != -1) {
            clear_bit(conf->bit_bufs[FILE_START_BLOCKS], mfs->open_file_match_index);

            res = scan_file(mfs, &end_index, mfs->open_file_match_index, conf->bit_bufs[SCRATCH_1]);
            if(res) return res;
            if(end_index < 0) return MFS_INTERNAL_ASSERTION_ERROR;

            int bit_buf_len = MFS_BIT_BUF_SIZE_BYTES(conf->block_count);
            for(int i = 0; i < bit_buf_len; i++) {
                conf->bit_bufs[OCCUPIED_BLOCKS][i] &= ~conf->bit_bufs[SCRATCH_1][i];
            }

            /* clobber the first page */
            memset(conf->block_buf, 0xff, conf->block_size);
            res = conf->write_block(conf->cb_ctx, mfs->open_file_match_index);
            if(res) return res;
            res = conf->read_block(conf->cb_ctx, mfs->open_file_match_index);
            if(res) return res;
            for(int i = 0; i < conf->block_size; i++) {
                if(conf->block_buf[i] != 0xff) return MFS_READBACK_ERROR;
            }
        }
        else {
            mfs->file_count += 1;
        }
    }

    mfs->open_file_mode = -1;
}

