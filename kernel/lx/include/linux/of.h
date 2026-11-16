/*
 * The device tree as Linux drivers read it. There is no flattened tree under XNU, and so far no
 * node either: the files of the driver that are built ask two questions of it, which
 * src/lx_bus.c answers with "no". The rest is declared for the driver's bus file, which reads
 * its own node's properties; those will be made from what the kernel says about the radio.
 * Cells are kept in the processor's byte order.
 */
#ifndef _LX_LINUX_OF_H
#define _LX_LINUX_OF_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/mod_devicetable.h>
#include <linux/fwnode.h>

typedef u32 phandle;

struct property {
	const char *name;
	int length;			/* bytes */
	const void *value;
};

struct device_node {
	const char *name;
	const char *full_name;
	phandle phandle;
	const struct property *properties;
	int nproperties;
	struct device_node *parent;
	struct device_node *child;
	struct device_node *sibling;
	struct fwnode_handle fwnode;
};

struct of_device_id {
	char name[32];
	char type[32];
	char compatible[128];
	const void *data;
};

const struct property *of_find_property(const struct device_node *np, const char *name, int *lenp);
const void *of_get_property(const struct device_node *np, const char *name, int *lenp);
int of_property_read_string(const struct device_node *np, const char *name, const char **out);
int of_property_read_u32_index(const struct device_node *np, const char *name, u32 index, u32 *out);
bool of_device_is_compatible(const struct device_node *np, const char *compatible);
bool of_machine_is_compatible(const char *compatible);
struct device_node *of_get_child_by_name(const struct device_node *np, const char *name);
struct device_node *of_find_node_by_phandle(phandle handle);
struct device_node *of_parse_phandle(const struct device_node *np, const char *name, int index);
const struct of_device_id *of_match_node(const struct of_device_id *matches, const struct device_node *np);

static inline int
of_property_read_u32(const struct device_node *np, const char *name, u32 *out)
{
	return (of_property_read_u32_index(np, name, 0, out));
}

static inline bool
of_property_present(const struct device_node *np, const char *name)
{
	return (of_find_property(np, name, NULL) != NULL);
}

static inline bool
of_property_read_bool(const struct device_node *np, const char *name)
{
	return (of_find_property(np, name, NULL) != NULL);
}

/* the nodes live as long as the layer does */
static inline struct device_node *of_node_get(struct device_node *np) { return (np); }
static inline void of_node_put(struct device_node *np) { }

#endif
