/*
 * A block device over an image holding only some of its blocks: how the real iOS root
 * volume is read on the phone, which cannot hold all of it.  Only with boot-arg
 * apfs_sparse=<offset in md0>.  See bsd/dev/redwood_sparse.c.
 */
#ifndef _SYS_REDWOOD_SPARSE_H_
#define _SYS_REDWOOD_SPARSE_H_

void redwood_sparse_init(void);
void redwood_sparse_report(void);

#endif /* _SYS_REDWOOD_SPARSE_H_ */
