#include "bptree.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"

typedef struct {
    int leaf_depth_set;
    size_t leaf_depth;
} ValidationState;

static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list args;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, args);
    va_end(args);
}

static BPTreeNode *create_node(int is_leaf)
{
    BPTreeNode *node = (BPTreeNode *)calloc(1U, sizeof(BPTreeNode));

    if (node == NULL) {
        return NULL;
    }

    node->is_leaf = is_leaf;
    return node;
}

static BPTreeNode *find_leaf_node(BPTreeNode *root, uint64_t key)
{
    BPTreeNode *node = root;

    while (node != NULL && !node->is_leaf) {
        size_t child_index = 0U;

        while (child_index < node->key_count && key >= node->keys[child_index]) {
            child_index++;
        }

        node = node->ptrs.children[child_index];
    }

    return node;
}

static int insert_into_leaf(BPTreeNode *leaf, uint64_t key, long row_offset)
{
    size_t insert_index = 0U;
    size_t i;

    while (insert_index < leaf->key_count && leaf->keys[insert_index] < key) {
        insert_index++;
    }

    if (insert_index < leaf->key_count && leaf->keys[insert_index] == key) {
        return STATUS_INDEX_ERROR;
    }

    for (i = leaf->key_count; i > insert_index; --i) {
        leaf->keys[i] = leaf->keys[i - 1U];
        leaf->ptrs.row_offsets[i] = leaf->ptrs.row_offsets[i - 1U];
    }

    leaf->keys[insert_index] = key;
    leaf->ptrs.row_offsets[insert_index] = row_offset;
    leaf->key_count += 1U;
    return STATUS_OK;
}

static int insert_into_internal_node(BPTreeNode *node,
                                     size_t left_child_index,
                                     uint64_t separator_key,
                                     BPTreeNode *right_child)
{
    size_t i;

    for (i = node->key_count; i > left_child_index; --i) {
        node->keys[i] = node->keys[i - 1U];
    }

    for (i = node->key_count + 1U; i > left_child_index + 1U; --i) {
        node->ptrs.children[i] = node->ptrs.children[i - 1U];
    }

    node->keys[left_child_index] = separator_key;
    node->ptrs.children[left_child_index + 1U] = right_child;
    if (right_child != NULL) {
        right_child->parent = node;
    }
    node->key_count += 1U;
    return STATUS_OK;
}

static int split_internal_and_insert(BPTree *tree,
                                     BPTreeNode *node,
                                     size_t left_child_index,
                                     uint64_t separator_key,
                                     BPTreeNode *right_child,
                                     char *errbuf, size_t errbuf_size);

static int insert_into_parent(BPTree *tree,
                              BPTreeNode *left,
                              uint64_t separator_key,
                              BPTreeNode *right,
                              char *errbuf, size_t errbuf_size)
{
    BPTreeNode *parent;
    size_t left_child_index = 0U;

    if (tree == NULL || left == NULL || right == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid parent insertion arguments");
        return STATUS_INDEX_ERROR;
    }

    parent = left->parent;
    if (parent == NULL) {
        BPTreeNode *new_root = create_node(0);

        if (new_root == NULL) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: out of memory");
            return STATUS_INDEX_ERROR;
        }

        new_root->keys[0] = separator_key;
        new_root->ptrs.children[0] = left;
        new_root->ptrs.children[1] = right;
        new_root->key_count = 1U;
        left->parent = new_root;
        right->parent = new_root;
        tree->root = new_root;
        return STATUS_OK;
    }

    while (left_child_index <= parent->key_count && parent->ptrs.children[left_child_index] != left) {
        left_child_index++;
    }

    if (left_child_index > parent->key_count) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: parent-child relationship is broken");
        return STATUS_INDEX_ERROR;
    }

    if (parent->key_count < BPTREE_MAX_KEYS) {
        return insert_into_internal_node(parent, left_child_index, separator_key, right);
    }

    return split_internal_and_insert(tree, parent, left_child_index, separator_key, right,
                                     errbuf, errbuf_size);
}

static int split_leaf_and_insert(BPTree *tree,
                                 BPTreeNode *leaf,
                                 uint64_t key,
                                 long row_offset,
                                 char *errbuf, size_t errbuf_size)
{
    uint64_t temp_keys[BPTREE_ORDER];
    long temp_offsets[BPTREE_ORDER];
    BPTreeNode *new_leaf;
    size_t insert_index = 0U;
    size_t total_keys;
    size_t left_count;
    size_t right_count;
    size_t i;

    if (tree == NULL || leaf == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid leaf split arguments");
        return STATUS_INDEX_ERROR;
    }

    while (insert_index < leaf->key_count && leaf->keys[insert_index] < key) {
        insert_index++;
    }

    if (insert_index < leaf->key_count && leaf->keys[insert_index] == key) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: duplicate key %" PRIu64, key);
        return STATUS_INDEX_ERROR;
    }

    for (i = 0U; i < insert_index; ++i) {
        temp_keys[i] = leaf->keys[i];
        temp_offsets[i] = leaf->ptrs.row_offsets[i];
    }

    temp_keys[insert_index] = key;
    temp_offsets[insert_index] = row_offset;

    for (i = insert_index; i < leaf->key_count; ++i) {
        temp_keys[i + 1U] = leaf->keys[i];
        temp_offsets[i + 1U] = leaf->ptrs.row_offsets[i];
    }

    new_leaf = create_node(1);
    if (new_leaf == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: out of memory");
        return STATUS_INDEX_ERROR;
    }

    total_keys = leaf->key_count + 1U;
    left_count = total_keys / 2U;
    right_count = total_keys - left_count;

    memset(leaf->keys, 0, sizeof(leaf->keys));
    memset(leaf->ptrs.row_offsets, 0, sizeof(leaf->ptrs.row_offsets));
    leaf->key_count = left_count;

    for (i = 0U; i < left_count; ++i) {
        leaf->keys[i] = temp_keys[i];
        leaf->ptrs.row_offsets[i] = temp_offsets[i];
    }

    for (i = 0U; i < right_count; ++i) {
        new_leaf->keys[i] = temp_keys[left_count + i];
        new_leaf->ptrs.row_offsets[i] = temp_offsets[left_count + i];
    }
    new_leaf->key_count = right_count;
    new_leaf->next = leaf->next;
    new_leaf->parent = leaf->parent;
    leaf->next = new_leaf;

    return insert_into_parent(tree, leaf, new_leaf->keys[0], new_leaf, errbuf, errbuf_size);
}

static int split_internal_and_insert(BPTree *tree,
                                     BPTreeNode *node,
                                     size_t left_child_index,
                                     uint64_t separator_key,
                                     BPTreeNode *right_child,
                                     char *errbuf, size_t errbuf_size)
{
    uint64_t temp_keys[BPTREE_ORDER];
    BPTreeNode *temp_children[BPTREE_ORDER + 1U];
    BPTreeNode *right_node;
    uint64_t promoted_key;
    size_t total_keys;
    size_t split_index;
    size_t i;
    size_t right_key_count;

    if (tree == NULL || node == NULL || right_child == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid internal split arguments");
        return STATUS_INDEX_ERROR;
    }

    for (i = 0U; i < left_child_index; ++i) {
        temp_keys[i] = node->keys[i];
    }
    temp_keys[left_child_index] = separator_key;
    for (i = left_child_index; i < node->key_count; ++i) {
        temp_keys[i + 1U] = node->keys[i];
    }

    for (i = 0U; i <= left_child_index; ++i) {
        temp_children[i] = node->ptrs.children[i];
    }
    temp_children[left_child_index + 1U] = right_child;
    for (i = left_child_index + 1U; i <= node->key_count; ++i) {
        temp_children[i + 1U] = node->ptrs.children[i];
    }

    right_node = create_node(0);
    if (right_node == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: out of memory");
        return STATUS_INDEX_ERROR;
    }

    total_keys = node->key_count + 1U;
    split_index = total_keys / 2U;
    promoted_key = temp_keys[split_index];

    memset(node->keys, 0, sizeof(node->keys));
    memset(node->ptrs.children, 0, sizeof(node->ptrs.children));
    node->key_count = split_index;

    for (i = 0U; i < split_index; ++i) {
        node->keys[i] = temp_keys[i];
    }
    for (i = 0U; i <= split_index; ++i) {
        node->ptrs.children[i] = temp_children[i];
        if (node->ptrs.children[i] != NULL) {
            node->ptrs.children[i]->parent = node;
        }
    }

    right_key_count = total_keys - split_index - 1U;
    right_node->key_count = right_key_count;
    right_node->parent = node->parent;

    for (i = 0U; i < right_key_count; ++i) {
        right_node->keys[i] = temp_keys[split_index + 1U + i];
    }
    for (i = 0U; i <= right_key_count; ++i) {
        right_node->ptrs.children[i] = temp_children[split_index + 1U + i];
        if (right_node->ptrs.children[i] != NULL) {
            right_node->ptrs.children[i]->parent = right_node;
        }
    }

    return insert_into_parent(tree, node, promoted_key, right_node, errbuf, errbuf_size);
}

static void destroy_node(BPTreeNode *node)
{
    size_t i;

    if (node == NULL) {
        return;
    }

    if (!node->is_leaf) {
        for (i = 0U; i <= node->key_count; ++i) {
            destroy_node(node->ptrs.children[i]);
        }
    }

    free(node);
}

static int validate_node(const BPTreeNode *node,
                         const BPTreeNode *expected_parent,
                         int has_lower, uint64_t lower_bound,
                         int has_upper, uint64_t upper_bound,
                         size_t depth,
                         ValidationState *state,
                         char *errbuf, size_t errbuf_size)
{
    size_t i;

    if (node == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: null node encountered");
        return STATUS_INDEX_ERROR;
    }

    if (node->parent != expected_parent) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid parent pointer");
        return STATUS_INDEX_ERROR;
    }

    if (node->key_count > BPTREE_MAX_KEYS) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: key count overflow");
        return STATUS_INDEX_ERROR;
    }

    for (i = 1U; i < node->key_count; ++i) {
        if (node->keys[i - 1U] >= node->keys[i]) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: keys are not strictly increasing");
            return STATUS_INDEX_ERROR;
        }
    }

    if (node->is_leaf) {
        for (i = 0U; i < node->key_count; ++i) {
            if ((has_lower && node->keys[i] < lower_bound) ||
                (has_upper && node->keys[i] >= upper_bound)) {
                set_error(errbuf, errbuf_size, "INDEX ERROR: leaf key outside allowed range");
                return STATUS_INDEX_ERROR;
            }
        }

        if (!state->leaf_depth_set) {
            state->leaf_depth = depth;
            state->leaf_depth_set = 1;
        } else if (state->leaf_depth != depth) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: leaf depth mismatch");
            return STATUS_INDEX_ERROR;
        }

        return STATUS_OK;
    }

    for (i = 0U; i <= node->key_count; ++i) {
        int child_has_lower = has_lower;
        int child_has_upper = has_upper;
        uint64_t child_lower = lower_bound;
        uint64_t child_upper = upper_bound;

        if (node->ptrs.children[i] == NULL) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: internal node has null child");
            return STATUS_INDEX_ERROR;
        }

        if (i > 0U) {
            child_has_lower = 1;
            child_lower = node->keys[i - 1U];
        }
        if (i < node->key_count) {
            child_has_upper = 1;
            child_upper = node->keys[i];
        }

        if (child_has_lower && child_has_upper && child_lower >= child_upper) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: invalid child key range");
            return STATUS_INDEX_ERROR;
        }

        if (validate_node(node->ptrs.children[i], node,
                          child_has_lower, child_lower,
                          child_has_upper, child_upper,
                          depth + 1U, state, errbuf, errbuf_size) != STATUS_OK) {
            return STATUS_INDEX_ERROR;
        }
    }

    return STATUS_OK;
}

static BPTreeNode *find_leftmost_leaf(BPTreeNode *root)
{
    BPTreeNode *node = root;

    while (node != NULL && !node->is_leaf) {
        node = node->ptrs.children[0];
    }

    return node;
}

int bptree_init(BPTree *out_tree, char *errbuf, size_t errbuf_size)
{
    if (out_tree == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid tree pointer");
        return STATUS_INDEX_ERROR;
    }

    out_tree->root = NULL;
    out_tree->key_count = 0U;
    if (errbuf != NULL && errbuf_size > 0U) {
        errbuf[0] = '\0';
    }
    return STATUS_OK;
}

int bptree_search(const BPTree *tree, uint64_t key,
                  long *out_offset, int *out_found,
                  char *errbuf, size_t errbuf_size)
{
    BPTreeNode *leaf;
    size_t i;

    if (out_offset != NULL) {
        *out_offset = 0L;
    }
    if (out_found != NULL) {
        *out_found = 0;
    }

    if (tree == NULL || out_offset == NULL || out_found == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid search arguments");
        return STATUS_INDEX_ERROR;
    }

    if (tree->root == NULL) {
        return STATUS_OK;
    }

    leaf = find_leaf_node(tree->root, key);
    if (leaf == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: failed to locate leaf");
        return STATUS_INDEX_ERROR;
    }

    for (i = 0U; i < leaf->key_count; ++i) {
        if (leaf->keys[i] == key) {
            *out_offset = leaf->ptrs.row_offsets[i];
            *out_found = 1;
            return STATUS_OK;
        }
    }

    return STATUS_OK;
}

int bptree_insert(BPTree *tree, uint64_t key, long row_offset,
                  char *errbuf, size_t errbuf_size)
{
    BPTreeNode *leaf;
    int status;

    if (tree == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid tree pointer");
        return STATUS_INDEX_ERROR;
    }

    if (tree->root == NULL) {
        tree->root = create_node(1);
        if (tree->root == NULL) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: out of memory");
            return STATUS_INDEX_ERROR;
        }

        tree->root->keys[0] = key;
        tree->root->ptrs.row_offsets[0] = row_offset;
        tree->root->key_count = 1U;
        tree->key_count = 1U;
        return STATUS_OK;
    }

    leaf = find_leaf_node(tree->root, key);
    if (leaf == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: failed to locate insertion leaf");
        return STATUS_INDEX_ERROR;
    }

    if (leaf->key_count < BPTREE_MAX_KEYS) {
        status = insert_into_leaf(leaf, key, row_offset);
        if (status != STATUS_OK) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: duplicate key %" PRIu64, key);
            return STATUS_INDEX_ERROR;
        }
    } else {
        status = split_leaf_and_insert(tree, leaf, key, row_offset, errbuf, errbuf_size);
        if (status != STATUS_OK) {
            return status;
        }
    }

    tree->key_count += 1U;
    return STATUS_OK;
}

int bptree_validate(const BPTree *tree, char *errbuf, size_t errbuf_size)
{
    ValidationState state = {0};
    BPTreeNode *leaf;
    size_t counted_keys = 0U;
    uint64_t previous_key = 0U;
    int has_previous = 0;

    if (tree == NULL) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: invalid tree pointer");
        return STATUS_INDEX_ERROR;
    }

    if (tree->root == NULL) {
        if (tree->key_count != 0U) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: empty tree has non-zero key count");
            return STATUS_INDEX_ERROR;
        }
        return STATUS_OK;
    }

    if (validate_node(tree->root, NULL, 0, 0U, 0, 0U, 0U, &state, errbuf, errbuf_size) != STATUS_OK) {
        return STATUS_INDEX_ERROR;
    }

    leaf = find_leftmost_leaf(tree->root);
    while (leaf != NULL) {
        size_t i;

        if (!leaf->is_leaf) {
            set_error(errbuf, errbuf_size, "INDEX ERROR: leaf chain contains internal node");
            return STATUS_INDEX_ERROR;
        }

        for (i = 0U; i < leaf->key_count; ++i) {
            if (has_previous && previous_key >= leaf->keys[i]) {
                set_error(errbuf, errbuf_size, "INDEX ERROR: leaf chain is not sorted");
                return STATUS_INDEX_ERROR;
            }
            previous_key = leaf->keys[i];
            has_previous = 1;
            counted_keys += 1U;
        }

        leaf = leaf->next;
    }

    if (counted_keys != tree->key_count) {
        set_error(errbuf, errbuf_size, "INDEX ERROR: key count mismatch");
        return STATUS_INDEX_ERROR;
    }

    return STATUS_OK;
}

void bptree_destroy(BPTree *tree)
{
    if (tree == NULL) {
        return;
    }

    destroy_node(tree->root);
    tree->root = NULL;
    tree->key_count = 0U;
}
