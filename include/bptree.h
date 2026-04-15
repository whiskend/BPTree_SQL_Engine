#ifndef BPTREE_H
#define BPTREE_H

#include <stddef.h>
#include <stdint.h>

#define BPTREE_ORDER 64
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)
#define BPTREE_MAX_CHILDREN (BPTREE_ORDER)

typedef struct BPTreeNode {
    int is_leaf;
    size_t key_count;
    uint64_t keys[BPTREE_MAX_KEYS];
    struct BPTreeNode *parent;
    struct BPTreeNode *next;
    union {
        struct BPTreeNode *children[BPTREE_MAX_CHILDREN];
        long row_offsets[BPTREE_MAX_KEYS];
    } ptrs;
} BPTreeNode;

typedef struct {
    BPTreeNode *root;
    size_t key_count;
} BPTree;

int bptree_init(BPTree *out_tree, char *errbuf, size_t errbuf_size);
int bptree_search(const BPTree *tree, uint64_t key,
                  long *out_offset, int *out_found,
                  char *errbuf, size_t errbuf_size);
int bptree_insert(BPTree *tree, uint64_t key, long row_offset,
                  char *errbuf, size_t errbuf_size);
int bptree_validate(const BPTree *tree, char *errbuf, size_t errbuf_size);
void bptree_destroy(BPTree *tree);

#endif
