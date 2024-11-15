# mcp_fs

A filesystem.

```c
#define MY_BLOCK_SIZE 2048
#define MY_BLOCK_COUNT 16
static uint8_t block_buf[MY_BLOCK_SIZE];
static int read_block_cb(void * cb_ctx, int block_index){
	my_read_block(block_buf, block_index);
	return 0;
}
static int write_block_cb(void * cb_ctx, int block_index) {
	my_write_block(block_buf, block_index);
	return 0;
}
static uint8_t bb[4][MFS_BIT_BUF_SIZE_BYTES(MY_BLOCK_COUNT)];
static const mfs_conf_t conf = {
	.block_buf = block_buf,
	.bit_bufs = {bb[0], bb[1], bb[2], bb[3]},
	.block_size = MY_BLOCK_SIZE,
	.block_count = MY_BLOCK_COUNT,
	.cb_ctx = NULL,
	.read_block = read_block_cb,
	.write_block = write_block_cb
};
static mfs_t mfs;
int err = mfs_mount(&mfs, &conf);
assert(err = 0);
err = mfs_open(&mfs, "foo.txt", MFS_MODE_WRITE);
assert(err = 0);
/* ... */
```

Properties:

- No directories
- Only one open file at a time.
- Mounting an existing volume
  requires that all the blocks
  are scanned. Only practical
  for small volumes.
- 1 file, 1 or more blocks.
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
