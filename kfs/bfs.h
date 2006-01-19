#ifndef __KUDOS_KFS_BFS_H
#define __KUDOS_KFS_BFS_H

#include <lib/types.h>

#include <kfs/oo.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>

struct BFS;
typedef struct BFS BFS_t;

/* "head" and "tail" parameters:
 * - The functions below that take head and tail parameters are mutators and
 * will pass back the changes they make using these pointers. The change
 * descriptor subgraphs they generate will be set up so that the head pointer
 * depends on the whole subgraph, and the tail pointer is depended upon by the
 * whole graph. Thus to make the resulting subgraph depend on something else,
 * make its tail depend on that. To make something else depend on the resulting
 * subgraph, make it depend on its head.
 * - head is both an input and output parameter: if input *head is non-NULL, the new
 * tail will be made to depend on it. tail is output only; it outputs the new tail. */

typedef inoden_t uint64_t

struct BFS {
	OBJECT(BFS_t);
	DECLARE(BFS_t, uint32_t, allocate_block, inoden_t file, int purpose, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(BFS_t, inoden_t, allocate_inode, uint8_t type, inoden_t link, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(BFS_t, int, append_file_block, inoden_t file, uint32_t block, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(BFS_t, int, cancel_synthetic_block, uint32_t number);
	DECLARE(BFS_t, int, free_block, inoden_t file, uint32_t block, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(BFS_t, BD_t *, get_blockdev);
	DECLARE(BFS_t, uint32_t, get_blocksize);
	DECLARE(BFS_t, uint32_t, get_file_numblocks, inoden_t file);
	DECLARE(BFS_t, uint32_t, get_file_block, inoden_t file, uint32_t offset);
	DECLARE(BFS_t, bdesc_t *, lookup_block, uint32_t number);
	DECLARE(BFS_t, int, remove_inode, inoden_t num, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(BFS_t, int, sync, inoden_t num);
	DECLARE(BFS_t, bdesc_t *, synthetic_lookup_block, uint32_t number, bool * synthetic);
	DECLARE(BFS_t, uint32_t, truncate_file_block, inoden_t file, chdesc_t ** head, chdesc_t ** tail);
	DECLARE(BFS_t, int, write_block, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail);
};

#define BFS_INIT(bfs, module, info) { \
	OBJ_INIT(bfs, module, info); \
	ASSIGN(bfs, module, allocate_block); \
	ASSIGN(bfs, module, allocate_inode); \
	ASSIGN(bfs, module, append_file_block); \
	ASSIGN(bfs, module, cancel_synthetic_block); \
	ASSIGN(bfs, module, free_block); \
	ASSIGN(bfs, module, get_blockdev); \
	ASSIGN(bfs, module, get_blocksize); \
	ASSIGN(bfs, module, get_file_block); \
	ASSIGN(bfs, module, get_file_numblocks); \
	ASSIGN(bfs, module, lookup_block); \
	ASSIGN(bfs, module, remove_inode); \
	ASSIGN(bfs, module, sync); \
	ASSIGN(bfs, module, synthetic_lookup_block); \
	ASSIGN(bfs, module, truncate_file_block); \
	ASSIGN(bfs, module, write_block); \
}

#endif /* __KUDOS_KFS_BFS_H */
