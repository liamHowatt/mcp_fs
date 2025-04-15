# mcp_fs

A simple, slow, ultracompact filesystem.

```c
#define MY_BLOCK_SIZE 2048
#define MY_BLOCK_COUNT 16
static int read_block(void * cb_ctx, int block_index, void * dst) {
    my_read_block(dst, block_index);
    return 0;
}
static int write_block(void * cb_ctx, int block_index, const void * src) {
    my_write_block(src, block_index);
    return 0;
}
static uint8_t aligned_aux_memory[MFS_ALIGNED_AUX_MEMORY_SIZE(BLOCK_SIZE, BLOCK_COUNT)] __attribute__((aligned));
static const mfs_conf_t conf = {
    aligned_aux_memory,
    BLOCK_SIZE,
    BLOCK_COUNT,
    cb_ctx,
    read_block,
    write_block
};
static mfs_t mfs;
int err = mfs_mount(&mfs, &conf);
assert(err == 0);
err = mfs_open(&mfs, "foo.txt", MFS_MODE_WRITE);
assert(err == 0);
/* ... */
```

Properties:

- No directories
- Only one open file at a time.
- Mounting an existing volume
  requires that all the blocks
  are scanned. Only practical
  for small volumes.
- 1 file == 1 or more blocks.
  No metadata blocks.
- A file can be stored across
  blocks that are not contiguous.
  This is fairly standard.
- Re-writing an existing file
  is power-failure tollerant.
  The changes are committed
  atomically. The original file
  contents will always be seen
  after a power interruption.
  Only a successful file close
  commits the new file contents.
