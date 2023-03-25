// SPDX-License-Identifier: GPL-2.0
//
// Register cache access API - maple tree based cache
//
// Copyright 2023 Arm, Ltd
//
// Author: Mark Brown <broonie@kernel.org>

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/maple_tree.h>
#include <linux/slab.h>

#include "internal.h"

struct cache_entry {
	unsigned int reg;
	unsigned int val;
};

static int regcache_maple_read(struct regmap *map,
			       unsigned int reg, unsigned int *value)
{
	struct maple_tree *mt = map->cache;
	struct cache_entry *entry;

	entry = mtree_load(mt, reg);
	if (!entry)
		return -ENOENT;

	*value = entry->val;

	return 0;
}

static int regcache_maple_write(struct regmap *map, unsigned int reg,
				unsigned int val)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, reg, reg);
	struct cache_entry *entry;
	int ret;

	entry = mas_find(&mas, reg);
	if (entry) {
		entry->val = val;
		return 0;
	}

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->reg = reg;
	entry->val = val;

	ret = mtree_store(mt, reg, entry, GFP_KERNEL);
	if (ret != 0)
		kfree(entry);

	return ret;
}

static int regcache_maple_drop(struct regmap *map, unsigned int min,
			       unsigned int max)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, min, max);
	struct cache_entry *entry;

	for (entry = mas_find(&mas, min); entry; entry = mas_next(&mas, max)) {
		kfree(entry);
		mas_erase(&mas);
	}

	return 0;
}

static int regcache_maple_sync(struct regmap *map, unsigned int min,
			       unsigned int max)
{
	struct maple_tree *mt = map->cache;
	struct cache_entry *entry;
	MA_STATE(mas, mt, min, max);
	int ret;

	map->cache_bypass = true;

	for (entry = mas_find(&mas, min); entry; entry = mas_next(&mas, max)) {
		ret = regcache_sync_val(map, entry->reg, entry->val);
		if (ret != 0)
			goto out;
	}

out:
	map->cache_bypass = false;

	return ret;
}

static int regcache_maple_exit(struct regmap *map)
{
	struct maple_tree *mt = map->cache;

	/* if we've already been called then just return */
	if (!mt)
		return 0;

	regcache_maple_drop(map, 0, UINT_MAX);

	kfree(mt);
	map->cache = NULL;

	return 0;
}

static int regcache_maple_init(struct regmap *map)
{
	struct maple_tree *mt;
	int i;
	int ret;

	mt = kmalloc(sizeof(*mt), GFP_KERNEL);
	if (!mt)
		return -ENOMEM;
	map->cache = mt;

	mt_init(mt);

	for (i = 0; i < map->num_reg_defaults; i++) {
		ret = regcache_maple_write(map,
					   map->reg_defaults[i].reg,
					   map->reg_defaults[i].def);
		if (ret)
			goto err;
	}

	return 0;

err:
	regcache_maple_exit(map);
	return ret;
}

struct regcache_ops regcache_maple_ops = {
	.type = REGCACHE_MAPLE,
	.name = "maple",
	.init = regcache_maple_init,
	.exit = regcache_maple_exit,
	.read = regcache_maple_read,
	.write = regcache_maple_write,
	.drop = regcache_maple_drop,
	.sync = regcache_maple_sync,
};
