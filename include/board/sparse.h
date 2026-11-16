/*
 * A block device over an image holding only some of its blocks: how the real Darwin root
 * volume is read on the phone, which cannot hold all of it.  Only with boot-arg
 * apfs_sparse=<offset in md0>.  See bsd/dev/board/sparse.c.
 */
#ifndef _SYS_HAKIOS_SPARSE_H_
#define _SYS_HAKIOS_SPARSE_H_

void hakios_sparse_init(void);
void hakios_sparse_report(void);

#endif /* _SYS_HAKIOS_SPARSE_H_ */
