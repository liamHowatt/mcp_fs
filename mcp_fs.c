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

#define SET_NEEDS_REMOUNT_THEN_RETURN(mfs, retval) do {mfs->needs_remount = true; return retval;} while(0)
#define SET_FILE_CLOSED_THEN_RETURN(mfs, retval) do {mfs->open_file_mode = -1; return retval;} while(0)

enum {
    FILE_START_BLOCKS,
    OCCUPIED_BLOCKS,
    SCRATCH_1,
    SCRATCH_2
};

static void set_bit(uint8_t * bit_buf, unsigned bit_index)
{
    bit_buf[bit_index / 8] |= 1 << (bit_index % 8);
}

static bool get_bit(const uint8_t * bit_buf, unsigned bit_index)
{
    return bit_buf[bit_index / 8] & (1 << (bit_index % 8));
}

static void clear_bit(uint8_t * bit_buf, unsigned bit_index)
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

static int scan_file(const mfs_t * mfs, int * end_index_dst, int block_index, uint8_t * scratch_bit_buf)
{
    int res;
    const mfs_conf_t * conf = mfs->conf;

    memset(scratch_bit_buf, 0, MFS_BIT_BUF_SIZE_BYTES(conf->block_count));
    uint32_t running_checksum = CHECKSUM_INIT_VAL;

    int current_block_index = block_index;
    while(1) {
        res = conf->read_block(conf->cb_ctx, current_block_index, mfs->block_buf);
        if(res) return res;
        set_bit(scratch_bit_buf, current_block_index);
        *end_index_dst = current_block_index;

        int32_t unoccupied_data_bytes;
        memcpy(&unoccupied_data_bytes, mfs->block_buf + (conf->block_size - 8), 4);
        bool has_next_block = unoccupied_data_bytes < 0;
        uint32_t next_block_or_target_checksum;
        memcpy(&next_block_or_target_checksum, mfs->block_buf + (conf->block_size - 4), 4);
        if(!has_next_block) {
            running_checksum = checksum_update(running_checksum, mfs->block_buf, conf->block_size - 4);
            if(running_checksum != next_block_or_target_checksum) {
                *end_index_dst = -1;
            }
            return 0;
        }
        if(next_block_or_target_checksum >= conf->block_count
            || get_bit(mfs->bit_bufs[OCCUPIED_BLOCKS], next_block_or_target_checksum)
            || get_bit(scratch_bit_buf, next_block_or_target_checksum)) {
            *end_index_dst = -1;
            return 0;
        }
        running_checksum = checksum_update(running_checksum, mfs->block_buf, conf->block_size);
        current_block_index = next_block_or_target_checksum;
    }
}

static int mount_inner(mfs_t * mfs, int file_initial_idx)
{
    int res;
    const mfs_conf_t * conf = mfs->conf;

    int file_end_idx_this;
    res = scan_file(mfs, &file_end_idx_this, file_initial_idx, mfs->bit_bufs[SCRATCH_1]);
    if(res) return res;
    if(file_end_idx_this < 0) {
        return 0;
    }

    res = conf->read_block(conf->cb_ctx, file_initial_idx, mfs->block_buf);
    if(res) return res;

    uint32_t birthday_this;
    memcpy(&birthday_this, mfs->block_buf, 4);

    int32_t preferred_if_older;
    memcpy(&preferred_if_older, mfs->block_buf + 4, 4);
    if(preferred_if_older < 0) {
        goto label_end_success;
    }

    int file_end_idx_other;
    res = scan_file(mfs, &file_end_idx_other, preferred_if_older, mfs->bit_bufs[SCRATCH_2]);
    if(res) return res;
    if(file_end_idx_other < 0) {
        goto label_end_success;
    }

    res = conf->read_block(conf->cb_ctx, preferred_if_older, mfs->block_buf);
    if(res) return res;

    uint32_t birthday_other;
    memcpy(&birthday_other, mfs->block_buf, 4);
    if(birthday_other <= birthday_this) {
        return 0;
    }

label_end_success:
    if(birthday_this > mfs->youngest) mfs->youngest = birthday_this;
    set_bit(mfs->bit_bufs[FILE_START_BLOCKS], file_initial_idx);
    mfs->file_count += 1;
    int bit_buf_len = MFS_BIT_BUF_SIZE_BYTES(conf->block_count);
    for(int i = 0; i < bit_buf_len; i++) {
        mfs->bit_bufs[OCCUPIED_BLOCKS][i] |= mfs->bit_bufs[SCRATCH_1][i];
    }
    return 0;
}

int mfs_mount(mfs_t * mfs, const mfs_conf_t * conf)
{
    int res;

    if(conf->block_size < (4 + 4 + 1 + 1 + 4 + 4)
       || conf->block_count < 1) {
        return MFS_BAD_BLOCK_CONFIG_ERROR;
    }

    int bit_buf_size = MFS_BIT_BUF_SIZE_BYTES(conf->block_count);

    mfs->conf = conf;

    mfs->block_buf = conf->aligned_aux_memory;
    uint8_t * aux_mem_u8 = conf->aligned_aux_memory;
    aux_mem_u8 += conf->block_size;
    for(int i = 0; i < 4; i++) {
        mfs->bit_bufs[i] = aux_mem_u8;
        aux_mem_u8 += bit_buf_size;
    }

    mfs->file_count = 0;
    mfs->youngest = 0;
    mfs->open_file_mode = -1;
    mfs->needs_remount = false;

    memset(mfs->bit_bufs[FILE_START_BLOCKS], 0, bit_buf_size);
    memset(mfs->bit_bufs[OCCUPIED_BLOCKS], 0, bit_buf_size);

    for(int file_initial_idx = 0; file_initial_idx < conf->block_count; file_initial_idx++) {
        res = mount_inner(mfs, file_initial_idx);
        if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res); /* a convenience for internal callers */
    }

    return 0;
}

int mfs_file_count(mfs_t * mfs)
{
    int res;

    if(mfs->needs_remount) if((res = mfs_mount(mfs, mfs->conf))) return res;

    if(mfs->open_file_mode != -1) {
        if(mfs->open_file_mode == MFS_MODE_WRITE) mfs->needs_remount = true;
        mfs->open_file_mode = -1;
        return MFS_WRONG_MODE_ERROR;
    }

    return mfs->file_count;
}

int mfs_list_files(mfs_t * mfs, void * list_file_cb_ctx, void (*list_file_cb)(void *, const char *))
{
    int res;

    if(mfs->needs_remount) if((res = mfs_mount(mfs, mfs->conf))) return res;

    if(mfs->open_file_mode != -1) {
        if(mfs->open_file_mode == MFS_MODE_WRITE) mfs->needs_remount = true;
        mfs->open_file_mode = -1;
        return MFS_WRONG_MODE_ERROR;
    }

    const mfs_conf_t * conf = mfs->conf;

    int files_left = mfs->file_count;
    for(int i = 0; files_left; i++) {
        if(!get_bit(mfs->bit_bufs[FILE_START_BLOCKS], i)) {
            continue;
        }
        res = conf->read_block(conf->cb_ctx, i, mfs->block_buf);
        if(res) return res;
        list_file_cb(list_file_cb_ctx, (char *) mfs->block_buf + 8);
        files_left--;
    }

    return 0;
}

int mfs_delete(mfs_t * mfs, const char * name)
{
    int res;

    if(mfs->needs_remount) if((res = mfs_mount(mfs, mfs->conf))) return res;

    if(mfs->open_file_mode != -1) {
        if(mfs->open_file_mode == MFS_MODE_WRITE) mfs->needs_remount = true;
        mfs->open_file_mode = -1;
        return MFS_WRONG_MODE_ERROR;
    }

    const mfs_conf_t * conf = mfs->conf;
    int i;

    int name_len = strlen(name);
    if(name_len > conf->block_size - (4 + 4 + 1 + 4 + 4)
       || name_len < 1) {
        return MFS_FILE_NAME_BAD_LEN_ERROR;
    }

    int files_left = mfs->file_count;
    for(i = 0; files_left; i++) {
        if(!get_bit(mfs->bit_bufs[FILE_START_BLOCKS], i)) {
            continue;
        }
        res = conf->read_block(conf->cb_ctx, i, mfs->block_buf);
        if(res) return res;

        if(0 == strcmp(name, (char *) mfs->block_buf + 8)) {
            break;
        }

        files_left--;
    }

    if(!files_left) {
        return MFS_FILE_NOT_FOUND_ERROR;
    }
    int delete_file_page_1 = i;

    clear_bit(mfs->bit_bufs[FILE_START_BLOCKS], delete_file_page_1);

    uint32_t birthday;
    memcpy(&birthday, mfs->block_buf, 4);
    if(birthday == mfs->youngest) mfs->youngest--;

    int end_idx;
    res = scan_file(mfs, &end_idx, delete_file_page_1, mfs->bit_bufs[SCRATCH_1]);
    if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);
    if(end_idx < 0) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, MFS_INTERNAL_ASSERTION_ERROR);

    int bit_buf_len = MFS_BIT_BUF_SIZE_BYTES(conf->block_count);
    for(i = 0; i < bit_buf_len; i++) {
        mfs->bit_bufs[OCCUPIED_BLOCKS][i] &= ~mfs->bit_bufs[SCRATCH_1][i];
    }

    /* clobber the first page */
    memset(mfs->block_buf, 0xff, conf->block_size);
    res = conf->write_block(conf->cb_ctx, delete_file_page_1, mfs->block_buf);
    if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);
    res = conf->read_block(conf->cb_ctx, delete_file_page_1, mfs->block_buf);
    if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);
    for(i = 0; i < conf->block_size; i++) {
        if(mfs->block_buf[i] != 0xff) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, MFS_READBACK_ERROR);
    }

    mfs->file_count -= 1;

    return 0;
}

int mfs_open(mfs_t * mfs, const char * name, mfs_mode_t mode)
{
    int res;

    if(mfs->needs_remount) if((res = mfs_mount(mfs, mfs->conf))) return res;

    if(mfs->open_file_mode != -1) {
        if(mfs->open_file_mode == MFS_MODE_WRITE) mfs->needs_remount = true;
        mfs->open_file_mode = -1;
        return MFS_WRONG_MODE_ERROR;
    }

    const mfs_conf_t * conf = mfs->conf;
    int i;

    int name_len = strlen(name);
    if(name_len > conf->block_size - (4 + 4 + 1 + 1 + 4 + 4)
       || name_len < 1) {
        return MFS_FILE_NAME_BAD_LEN_ERROR;
    }

    int files_left = mfs->file_count;
    for(i = 0; files_left; i++) {
        if(!get_bit(mfs->bit_bufs[FILE_START_BLOCKS], i)) {
            continue;
        }
        res = conf->read_block(conf->cb_ctx, i, mfs->block_buf);
        if(res) return res;

        if(0 == strcmp(name, (char *) mfs->block_buf + 8)) {
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
            if(!get_bit(mfs->bit_bufs[OCCUPIED_BLOCKS], i)) break;
        }
        if(i == conf->block_count) {
            return MFS_NO_SPACE_ERROR;
        }
        set_bit(mfs->bit_bufs[OCCUPIED_BLOCKS], i);
        set_bit(mfs->bit_bufs[FILE_START_BLOCKS], i);
        if(mfs->youngest == UINT32_MAX) {
            SET_NEEDS_REMOUNT_THEN_RETURN(mfs, MFS_BIRTHDAY_LIMIT_REACHED_ERROR);
        }
        mfs->youngest += 1;
        memcpy(mfs->block_buf, &mfs->youngest, 4);
        memcpy(mfs->block_buf + 4, &mfs->open_file_match_index, 4);
        strcpy((char *) mfs->block_buf + 8, name);
        mfs->writer_checksum = checksum_update(CHECKSUM_INIT_VAL, mfs->block_buf, 8 + name_len + 1);
        mfs->open_file_block = i;
        mfs->open_file_first_block = i;
    }

    mfs->open_file_block_cursor = 8 + name_len + 1;

    mfs->open_file_mode = mode;
    return 0;
}

int mfs_read(mfs_t * mfs, uint8_t * dst, int size)
{
    if(mfs->needs_remount) return MFS_WRONG_MODE_ERROR;

    if(mfs->open_file_mode != MFS_MODE_READ) {
        if(mfs->open_file_mode == MFS_MODE_WRITE) mfs->needs_remount = true;
        return MFS_WRONG_MODE_ERROR;
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;

    int total_read = 0;

    while(size) {
        int32_t unoccupied_data_bytes;
        memcpy(&unoccupied_data_bytes, mfs->block_buf + (conf->block_size - 8), 4);
        bool has_next_block = unoccupied_data_bytes < 0;
        if(has_next_block) unoccupied_data_bytes = 0;

        int block_len_remaining = conf->block_size - mfs->open_file_block_cursor - unoccupied_data_bytes - 8;

        if(!block_len_remaining) {
            if(!has_next_block) {
                break;
            }

            uint32_t new_block_idx;
            memcpy(&new_block_idx, mfs->block_buf + (conf->block_size - 4), 4);

            res = conf->read_block(conf->cb_ctx, new_block_idx, mfs->block_buf);
            if(res) SET_FILE_CLOSED_THEN_RETURN(mfs, res);

            mfs->open_file_block_cursor = 0;

            memcpy(&unoccupied_data_bytes, mfs->block_buf + (conf->block_size - 8), 4);
            if(unoccupied_data_bytes < 0) unoccupied_data_bytes = 0;
            block_len_remaining = conf->block_size  - unoccupied_data_bytes - 8;
        }

        int copy_amount = block_len_remaining < size ? block_len_remaining : size;

        memcpy(dst, &mfs->block_buf[mfs->open_file_block_cursor], copy_amount);

        size -= copy_amount;
        dst += copy_amount;
        mfs->open_file_block_cursor += copy_amount;
        total_read += copy_amount;
    }

    return total_read;
}

int mfs_write(mfs_t * mfs, const uint8_t * src, int size)
{
    if(mfs->needs_remount) return MFS_WRONG_MODE_ERROR;

    if(mfs->open_file_mode != MFS_MODE_WRITE) {
        SET_FILE_CLOSED_THEN_RETURN(mfs, MFS_WRONG_MODE_ERROR);
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;

    int write_size_left = size;

    while(write_size_left) {
        int block_len_remaining = conf->block_size - mfs->open_file_block_cursor - 8;

        if(!block_len_remaining) {
            uint32_t i;
            for(i = 0; i < conf->block_count; i++) {
                if(!get_bit(mfs->bit_bufs[OCCUPIED_BLOCKS], i)) break;
            }
            if(i == conf->block_count) {
                SET_NEEDS_REMOUNT_THEN_RETURN(mfs, MFS_NO_SPACE_ERROR);
            }
            set_bit(mfs->bit_bufs[OCCUPIED_BLOCKS], i);

            int32_t unoccupied_data_bytes = -1;
            memcpy(mfs->block_buf + (conf->block_size - 8), &unoccupied_data_bytes, 4);
            memcpy(mfs->block_buf + (conf->block_size - 4), &i, 4);
            mfs->writer_checksum = checksum_update(mfs->writer_checksum, mfs->block_buf + (conf->block_size - 8), 8);

            res = conf->write_block(conf->cb_ctx, mfs->open_file_block, mfs->block_buf);
            if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);

            mfs->open_file_block_cursor = 0;
            mfs->open_file_block = i;
            block_len_remaining = conf->block_size - 8;
        }

        int copy_amount = block_len_remaining < write_size_left ? block_len_remaining : write_size_left;

        mfs->writer_checksum = checksum_update(mfs->writer_checksum, src, copy_amount);
        memcpy(mfs->block_buf + mfs->open_file_block_cursor, src, copy_amount);

        write_size_left -= copy_amount;
        src += copy_amount;
        mfs->open_file_block_cursor += copy_amount;
    }

    return size;
}

int mfs_close(mfs_t * mfs)
{
    if(mfs->needs_remount) return MFS_WRONG_MODE_ERROR;

    if(mfs->open_file_mode == -1) {
        return MFS_WRONG_MODE_ERROR;
    }

    int res;
    const mfs_conf_t * conf = mfs->conf;

    if(mfs->open_file_mode == MFS_MODE_WRITE) {
        int32_t unoccupied_data_bytes = conf->block_size - mfs->open_file_block_cursor - 8;
        memset(mfs->block_buf + mfs->open_file_block_cursor, 0xff, unoccupied_data_bytes);
        memcpy(mfs->block_buf + (conf->block_size - 8), &unoccupied_data_bytes, 4);
        mfs->writer_checksum = checksum_update(mfs->writer_checksum, mfs->block_buf + mfs->open_file_block_cursor, unoccupied_data_bytes + 4);
        memcpy(mfs->block_buf + (conf->block_size - 4), &mfs->writer_checksum, 4);

        res = conf->write_block(conf->cb_ctx, mfs->open_file_block, mfs->block_buf);
        if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);

        int end_index;
        res = scan_file(mfs, &end_index, mfs->open_file_first_block, mfs->bit_bufs[SCRATCH_1]);
        if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);

        if(mfs->open_file_match_index != -1) {
            clear_bit(mfs->bit_bufs[FILE_START_BLOCKS], mfs->open_file_match_index);

            res = scan_file(mfs, &end_index, mfs->open_file_match_index, mfs->bit_bufs[SCRATCH_1]);
            if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);
            if(end_index < 0) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, MFS_INTERNAL_ASSERTION_ERROR);

            int bit_buf_len = MFS_BIT_BUF_SIZE_BYTES(conf->block_count);
            for(int i = 0; i < bit_buf_len; i++) {
                mfs->bit_bufs[OCCUPIED_BLOCKS][i] &= ~mfs->bit_bufs[SCRATCH_1][i];
            }

            /* clobber the first page */
            memset(mfs->block_buf, 0xff, conf->block_size);
            res = conf->write_block(conf->cb_ctx, mfs->open_file_match_index, mfs->block_buf);
            if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);
            res = conf->read_block(conf->cb_ctx, mfs->open_file_match_index, mfs->block_buf);
            if(res) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, res);
            for(int i = 0; i < conf->block_size; i++) {
                if(mfs->block_buf[i] != 0xff) SET_NEEDS_REMOUNT_THEN_RETURN(mfs, MFS_READBACK_ERROR);
            }
        }
        else {
            mfs->file_count += 1;
        }
    }

    mfs->open_file_mode = -1;

    return 0;
}

