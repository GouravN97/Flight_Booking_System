#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "btree_algos.h"
#include "btree_internal.h"
#include "btree_leaf.h"
#include "btree_node.h"
#include "db.h"

#define INTERNAL_NODE_MAX_CHILDREN (INTERNAL_NODE_MAX_KEYS + 1)
#define INTERNAL_NODE_MIN_CHILDREN ((INTERNAL_NODE_MAX_CHILDREN + 1) / 2)
#define CHILD_INDEX_NOT_FOUND UINT32_MAX

static uint32_t leaf_node_min_cells(Db* db) {
  return (leaf_node_max_cells(db) + 1) / 2;
}

static bool find_existing_value(Db* db, uint32_t key, Cursor** out_cursor,
                                void** out_node) {
  Cursor* cursor = table_find(db, key);
  void* node = get_page(db->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  if (cursor->cell_num < num_cells &&
      *leaf_node_key(db, node, cursor->cell_num) == key) {
    *out_cursor = cursor;
    *out_node = node;
    return true;
  }

  free(cursor);
  *out_cursor = NULL;
  *out_node = NULL;
  return false;
}

static uint32_t internal_node_child_count(void* node) {
  return *internal_node_num_keys(node) + 1;
}

static void collect_internal_node_children(void* node, uint32_t* children,
                                           uint32_t* child_count) {
  uint32_t num_keys = *internal_node_num_keys(node);

  for (uint32_t i = 0; i < num_keys; i++) {
    children[i] = *internal_node_child(node, i);
  }

  children[num_keys] = *internal_node_right_child(node);
  *child_count = num_keys + 1;
}

static uint32_t internal_node_child_index(void* node, uint32_t child_page_num) {
  uint32_t child_count = internal_node_child_count(node);

  for (uint32_t i = 0; i < child_count; i++) {
    if (*internal_node_child(node, i) == child_page_num) {
      return i;
    }
  }

  return CHILD_INDEX_NOT_FOUND;
}

static void refresh_internal_node_keys(Db* db, uint32_t node_page_num) {
  void* node = get_page(db->pager, node_page_num);
  uint32_t num_keys = *internal_node_num_keys(node);

  for (uint32_t i = 0; i < num_keys; i++) {
    uint32_t child_page_num = *internal_node_child(node, i);
    void* child = get_page(db->pager, child_page_num);
    *internal_node_key(node, i) = get_node_max_key(db, child);
  }
}

static void refresh_ancestors_after_child_change(Db* db,
                                                 uint32_t child_page_num) {
  void* child = get_page(db->pager, child_page_num);
  if (is_node_root(child)) {
    return;
  }

  uint32_t parent_page_num = *node_parent(child);
  void* parent = get_page(db->pager, parent_page_num);
  uint32_t parent_old_max = get_node_max_key(db, parent);

  refresh_internal_node_keys(db, parent_page_num);

  parent = get_page(db->pager, parent_page_num);
  uint32_t parent_new_max = get_node_max_key(db, parent);
  if (parent_new_max != parent_old_max) {
    refresh_ancestors_after_child_change(db, parent_page_num);
  }
}

static void rewrite_internal_node_children(Db* db, uint32_t node_page_num,
                                           const uint32_t* children,
                                           uint32_t child_count) {
  void* node = get_page(db->pager, node_page_num);

  if (child_count == 0) {
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
    return;
  }

  *internal_node_num_keys(node) = child_count - 1;

  for (uint32_t i = 0; i + 1 < child_count; i++) {
    uint32_t child_page_num = children[i];
    void* child = get_page(db->pager, child_page_num);

    *internal_node_child(node, i) = child_page_num;
    *node_parent(child) = node_page_num;
    *internal_node_key(node, i) = get_node_max_key(db, child);
  }

  uint32_t right_child_page_num = children[child_count - 1];
  void* right_child = get_page(db->pager, right_child_page_num);
  *internal_node_right_child(node) = right_child_page_num;
  *node_parent(right_child) = node_page_num;
}

static void reparent_internal_node_children(Db* db, uint32_t node_page_num) {
  void* node = get_page(db->pager, node_page_num);
  if (get_node_type(node) != NODE_INTERNAL) {
    return;
  }

  uint32_t children[INTERNAL_NODE_MAX_CHILDREN];
  uint32_t child_count;
  collect_internal_node_children(node, children, &child_count);

  for (uint32_t i = 0; i < child_count; i++) {
    void* child = get_page(db->pager, children[i]);
    *node_parent(child) = node_page_num;
  }
}

static void collapse_root_if_needed(Db* db) {
  void* root = get_page(db->pager, db->root_page_num);
  if (get_node_type(root) != NODE_INTERNAL ||
      *internal_node_num_keys(root) != 0) {
    return;
  }

  uint32_t child_page_num = *internal_node_right_child(root);
  if (child_page_num == INVALID_PAGE_NUM) {
    return;
  }

  void* child = get_page(db->pager, child_page_num);
  memcpy(root, child, PAGE_SIZE);
  set_node_root(root, true);
  reparent_internal_node_children(db, db->root_page_num);
  pager_free_page(db->pager, child_page_num);
}

static void rebalance_internal_node(Db* db, uint32_t node_page_num);

static void remove_child_from_internal_node(Db* db, uint32_t parent_page_num,
                                            uint32_t child_page_num) {
  void* parent = get_page(db->pager, parent_page_num);
  uint32_t old_children[INTERNAL_NODE_MAX_CHILDREN];
  uint32_t new_children[INTERNAL_NODE_MAX_CHILDREN];
  uint32_t old_child_count;
  uint32_t new_child_count = 0;

  collect_internal_node_children(parent, old_children, &old_child_count);

  for (uint32_t i = 0; i < old_child_count; i++) {
    if (old_children[i] != child_page_num) {
      new_children[new_child_count] = old_children[i];
      new_child_count++;
    }
  }

  rewrite_internal_node_children(db, parent_page_num, new_children,
                                 new_child_count);

  parent = get_page(db->pager, parent_page_num);
  if (is_node_root(parent)) {
    collapse_root_if_needed(db);
  } else {
    rebalance_internal_node(db, parent_page_num);
  }
}

static void rebalance_internal_node(Db* db, uint32_t node_page_num) {
  void* node = get_page(db->pager, node_page_num);

  if (is_node_root(node)) {
    collapse_root_if_needed(db);
    return;
  }

  uint32_t node_children[INTERNAL_NODE_MAX_CHILDREN];
  uint32_t node_child_count;
  collect_internal_node_children(node, node_children, &node_child_count);

  if (node_child_count >= INTERNAL_NODE_MIN_CHILDREN) {
    refresh_ancestors_after_child_change(db, node_page_num);
    return;
  }

  uint32_t parent_page_num = *node_parent(node);
  void* parent = get_page(db->pager, parent_page_num);
  uint32_t node_index = internal_node_child_index(parent, node_page_num);
  uint32_t parent_child_count = internal_node_child_count(parent);

  uint32_t left_page_num = INVALID_PAGE_NUM;
  uint32_t right_page_num = INVALID_PAGE_NUM;
  void* left = NULL;
  void* right = NULL;
  uint32_t left_children[INTERNAL_NODE_MAX_CHILDREN];
  uint32_t right_children[INTERNAL_NODE_MAX_CHILDREN];
  uint32_t left_child_count = 0;
  uint32_t right_child_count = 0;

  if (node_index > 0 && node_index != CHILD_INDEX_NOT_FOUND) {
    left_page_num = *internal_node_child(parent, node_index - 1);
    left = get_page(db->pager, left_page_num);
    collect_internal_node_children(left, left_children, &left_child_count);
  }

  if (node_index != CHILD_INDEX_NOT_FOUND &&
      node_index + 1 < parent_child_count) {
    right_page_num = *internal_node_child(parent, node_index + 1);
    right = get_page(db->pager, right_page_num);
    collect_internal_node_children(right, right_children, &right_child_count);
  }

  if (left != NULL && left_child_count > INTERNAL_NODE_MIN_CHILDREN) {
    uint32_t borrowed_child = left_children[left_child_count - 1];
    uint32_t new_node_children[INTERNAL_NODE_MAX_CHILDREN];

    new_node_children[0] = borrowed_child;
    for (uint32_t i = 0; i < node_child_count; i++) {
      new_node_children[i + 1] = node_children[i];
    }

    rewrite_internal_node_children(db, left_page_num, left_children,
                                   left_child_count - 1);
    rewrite_internal_node_children(db, node_page_num, new_node_children,
                                   node_child_count + 1);
    refresh_ancestors_after_child_change(db, left_page_num);
    refresh_ancestors_after_child_change(db, node_page_num);
    return;
  }

  if (right != NULL && right_child_count > INTERNAL_NODE_MIN_CHILDREN) {
    uint32_t borrowed_child = right_children[0];
    uint32_t new_right_children[INTERNAL_NODE_MAX_CHILDREN];

    node_children[node_child_count] = borrowed_child;
    for (uint32_t i = 1; i < right_child_count; i++) {
      new_right_children[i - 1] = right_children[i];
    }

    rewrite_internal_node_children(db, right_page_num, new_right_children,
                                   right_child_count - 1);
    rewrite_internal_node_children(db, node_page_num, node_children,
                                   node_child_count + 1);
    refresh_ancestors_after_child_change(db, right_page_num);
    refresh_ancestors_after_child_change(db, node_page_num);
    return;
  }

  if (left != NULL) {
    for (uint32_t i = 0; i < node_child_count; i++) {
      left_children[left_child_count + i] = node_children[i];
    }

    rewrite_internal_node_children(db, left_page_num, left_children,
                                   left_child_count + node_child_count);
    remove_child_from_internal_node(db, parent_page_num, node_page_num);
    pager_free_page(db->pager, node_page_num);
    return;
  }

  if (right != NULL) {
    for (uint32_t i = 0; i < right_child_count; i++) {
      node_children[node_child_count + i] = right_children[i];
    }

    rewrite_internal_node_children(db, node_page_num, node_children,
                                   node_child_count + right_child_count);
    remove_child_from_internal_node(db, parent_page_num, right_page_num);
    pager_free_page(db->pager, right_page_num);
  }
}

static void merge_leaf_nodes(Db* db, uint32_t left_page_num,
                             uint32_t right_page_num) {
  void* left = get_page(db->pager, left_page_num);
  void* right = get_page(db->pager, right_page_num);
  uint32_t left_num_cells = *leaf_node_num_cells(left);
  uint32_t right_num_cells = *leaf_node_num_cells(right);
  uint32_t cell_size = leaf_node_cell_size(db);

  for (uint32_t i = 0; i < right_num_cells; i++) {
    memcpy(leaf_node_cell(db, left, left_num_cells + i),
           leaf_node_cell(db, right, i), cell_size);
  }

  *leaf_node_num_cells(left) = left_num_cells + right_num_cells;
  *leaf_node_next_leaf(left) = *leaf_node_next_leaf(right);

  uint32_t parent_page_num = *node_parent(right);
  remove_child_from_internal_node(db, parent_page_num, right_page_num);
  pager_free_page(db->pager, right_page_num);
}

static void rebalance_leaf_node(Db* db, uint32_t leaf_page_num) {
  void* leaf = get_page(db->pager, leaf_page_num);

  if (is_node_root(leaf)) {
    return;
  }

  uint32_t leaf_num_cells = *leaf_node_num_cells(leaf);
  uint32_t min_cells = leaf_node_min_cells(db);
  uint32_t cell_size = leaf_node_cell_size(db);

  if (leaf_num_cells >= min_cells) {
    refresh_ancestors_after_child_change(db, leaf_page_num);
    return;
  }

  uint32_t parent_page_num = *node_parent(leaf);
  void* parent = get_page(db->pager, parent_page_num);
  uint32_t leaf_index = internal_node_child_index(parent, leaf_page_num);
  uint32_t parent_child_count = internal_node_child_count(parent);

  uint32_t left_page_num = INVALID_PAGE_NUM;
  uint32_t right_page_num = INVALID_PAGE_NUM;
  void* left = NULL;
  void* right = NULL;
  uint32_t left_num_cells = 0;
  uint32_t right_num_cells = 0;

  if (leaf_index > 0 && leaf_index != CHILD_INDEX_NOT_FOUND) {
    left_page_num = *internal_node_child(parent, leaf_index - 1);
    left = get_page(db->pager, left_page_num);
    left_num_cells = *leaf_node_num_cells(left);
  }

  if (leaf_index != CHILD_INDEX_NOT_FOUND &&
      leaf_index + 1 < parent_child_count) {
    right_page_num = *internal_node_child(parent, leaf_index + 1);
    right = get_page(db->pager, right_page_num);
    right_num_cells = *leaf_node_num_cells(right);
  }

  if (left != NULL && left_num_cells > min_cells) {
    for (uint32_t i = leaf_num_cells; i > 0; i--) {
      memcpy(leaf_node_cell(db, leaf, i), leaf_node_cell(db, leaf, i - 1),
             cell_size);
    }

    memcpy(leaf_node_cell(db, leaf, 0),
           leaf_node_cell(db, left, left_num_cells - 1), cell_size);
    *leaf_node_num_cells(left) = left_num_cells - 1;
    *leaf_node_num_cells(leaf) = leaf_num_cells + 1;
    refresh_ancestors_after_child_change(db, left_page_num);
    refresh_ancestors_after_child_change(db, leaf_page_num);
    return;
  }

  if (right != NULL && right_num_cells > min_cells) {
    memcpy(leaf_node_cell(db, leaf, leaf_num_cells),
           leaf_node_cell(db, right, 0), cell_size);

    for (uint32_t i = 0; i + 1 < right_num_cells; i++) {
      memcpy(leaf_node_cell(db, right, i), leaf_node_cell(db, right, i + 1),
             cell_size);
    }

    *leaf_node_num_cells(right) = right_num_cells - 1;
    *leaf_node_num_cells(leaf) = leaf_num_cells + 1;
    refresh_ancestors_after_child_change(db, right_page_num);
    refresh_ancestors_after_child_change(db, leaf_page_num);
    return;
  }

  if (left != NULL) {
    merge_leaf_nodes(db, left_page_num, leaf_page_num);
    return;
  }

  if (right != NULL) {
    merge_leaf_nodes(db, leaf_page_num, right_page_num);
  }
}

DbResult create(Db* db, uint32_t key, const void* value) {
  if (db == NULL || value == NULL) {
    return DB_INVALID_ARGUMENT;
  }

  Cursor* cursor = table_find(db, key);
  void* node = get_page(db->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  if (cursor->cell_num < num_cells &&
      *leaf_node_key(db, node, cursor->cell_num) == key) {
    free(cursor);
    return DB_DUPLICATE_KEY;
  }

  leaf_node_insert(cursor, key, value);
  free(cursor);
  return DB_OK;
}

DbResult get(Db* db, uint32_t key, void* out_value) {
  if (db == NULL || out_value == NULL) {
    return DB_INVALID_ARGUMENT;
  }

  Cursor* cursor;
  void* node;
  if (!find_existing_value(db, key, &cursor, &node)) {
    return DB_NOT_FOUND;
  }

  memcpy(out_value, leaf_node_value(db, node, cursor->cell_num),
         db->value_size);
  free(cursor);
  return DB_OK;
}

DbResult update(Db* db, uint32_t key, const void* value) {
  if (db == NULL || value == NULL) {
    return DB_INVALID_ARGUMENT;
  }

  Cursor* cursor;
  void* node;
  if (!find_existing_value(db, key, &cursor, &node)) {
    return DB_NOT_FOUND;
  }

  memcpy(leaf_node_value(db, node, cursor->cell_num), value, db->value_size);
  free(cursor);
  return DB_OK;
}

DbResult delete(Db* db, uint32_t key) {
  if (db == NULL) {
    return DB_INVALID_ARGUMENT;
  }

  Cursor* cursor;
  void* node;
  if (!find_existing_value(db, key, &cursor, &node)) {
    return DB_NOT_FOUND;
  }

  uint32_t num_cells = *leaf_node_num_cells(node);
  uint32_t cell_size = leaf_node_cell_size(db);
  for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
    memcpy(leaf_node_cell(db, node, i), leaf_node_cell(db, node, i + 1),
           cell_size);
  }

  *leaf_node_num_cells(node) = num_cells - 1;
  rebalance_leaf_node(db, cursor->page_num);

  free(cursor);
  return DB_OK;
}

const char* db_result_message(DbResult result) {
  switch (result) {
    case DB_OK:
      return "ok";
    case DB_NOT_FOUND:
      return "not found";
    case DB_DUPLICATE_KEY:
      return "duplicate key";
    case DB_INVALID_ARGUMENT:
      return "invalid argument";
    case DB_UNSUPPORTED_OPERATION:
      return "unsupported operation";
  }

  return "unknown result";
}
