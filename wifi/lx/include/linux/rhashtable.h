/*
 * Linux's hash table that resizes itself, as far as the driver uses it: things found by a key
 * that lies inside them (peer.c: a peer by its address, and by its number). LinuxKPI's header
 * has the names and nothing behind them -- its rhashtable_init() fails --, so this one stands
 * in its place: a fixed number of chains, which never resize. The functions are in
 * src/lx_base.c.
 */
#ifndef _LX_LINUX_RHASHTABLE_H
#define _LX_LINUX_RHASHTABLE_H

#include <linux/types.h>

struct rhash_head {
	struct rhash_head *next;
};

struct rhashtable_params {
	uint16_t	head_offset;		/* of the thing's rhash_head, in the thing */
	uint16_t	key_len;
	uint16_t	key_offset;		/* of the key, in the thing */
	uint16_t	nelem_hint;
	bool		automatic_shrinking;
};

struct rhashtable {
	struct rhash_head *chains[64];
};

int	 rhashtable_init(struct rhashtable *table, const struct rhashtable_params *params);
void	 rhashtable_destroy(struct rhashtable *table);
/* the thing with that key, or NULL */
void	*rhashtable_lookup_fast(struct rhashtable *table, const void *key, const struct rhashtable_params params);
/* the thing already there under the new thing's key; or NULL, and the new thing is in */
void	*rhashtable_lookup_get_insert_fast(struct rhashtable *table, struct rhash_head *head,
	    const struct rhashtable_params params);
int	 rhashtable_remove_fast(struct rhashtable *table, struct rhash_head *head, const struct rhashtable_params params);

#endif
