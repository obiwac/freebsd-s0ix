/*
 * Copyright (c) 2021 Baptiste Daroussin <bapt@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _HASH_H
#define _HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * Allocation error handling for hash table operations.
 * Define HASH_ALLOC_ERROR before including this header to override
 * the default abort() behaviour (e.g., longjmp to a recovery point).
 */
#ifndef HASH_ALLOC_ERROR
#define HASH_ALLOC_ERROR abort()
#endif

typedef struct hash hash_t;

hash_t *hash_new(void);
void hash_destroy(hash_t *table);
bool hash_add(hash_t *table, const char *key, void *value,
    void (*free_func)(void *));
size_t hash_count(hash_t *table);

typedef struct {
	char	*key;
	void	*value;
	hash_t	*_table;
	size_t	 _index;
} hash_it;

typedef struct {
	char	*key;
	void	*value;
	void	(*free_func)(void *);
	bool	 tombstone;	/* Deleted entry; probe chain continues */
} hash_entry;

hash_entry	*hash_get(hash_t *table, const char *key);
void		*hash_get_value(hash_t *table, const char *key);
hash_it		 hash_iterator(hash_t *table);
bool		 hash_next(hash_it *it);
bool		 hash_del(hash_t *h, const char *key);
void		*hash_delete(hash_t *h, const char *key);

#define hash_safe_add(_t, _k, _v, _free_func) do {	\
	if ((_t) == NULL)				\
		(_t) = hash_new();			\
	else if (hash_get((_t), (_k)) != NULL)		\
		break;					\
	hash_add((_t), (_k), (_v), (_free_func));	\
} while (0)

#endif /* !_HASH_H */
