#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Visit every node and property: prop == 0 announces a node (root has
 * depth 1 and an empty name), otherwise val/len is the raw big-endian value.
 * Returns 0 on a bad header.
 */
typedef void (*fdt_cb)(void *ctx, int depth, const char *node, const char *prop,
		       uintptr_t val, uint32_t len);
int fdt_walk(uintptr_t fdt, fdt_cb cb, void *ctx);
uint32_t fdt_be32(uintptr_t a);
