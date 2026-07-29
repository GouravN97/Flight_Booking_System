#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btree_algos.h"
#include "btree_internal.h"
#include "btree_leaf.h"
#include "btree_node.h"

void initialize_leaf_node(void* node) {
  set_node_type(node, NODE_LEAF);
  set_node_root(node, false);
  *leaf_node_num_cells(node) = 0;
  *leaf_node_next_leaf(node) = 0;  // 0 represents no sibling
}


uint32_t* leaf_node_num_cells(void* node) {
  return node + LEAF_NODE_NUM_CELLS_OFFSET;
}

uint32_t* leaf_node_next_leaf(void* node) {
  return node + LEAF_NODE_NEXT_LEAF_OFFSET;
}

uint32_t leaf_node_cell_size_for_value_size(uint32_t value_size) {
  return LEAF_NODE_KEY_SIZE + value_size;
}

uint32_t leaf_node_cell_size(const Table* table) {
  return leaf_node_cell_size_for_value_size(table->value_size);
}

uint32_t leaf_node_max_cells(const Table* table) {
  return LEAF_NODE_SPACE_FOR_CELLS / leaf_node_cell_size(table);
}

uint32_t leaf_node_right_split_count(const Table* table) {
  return (leaf_node_max_cells(table) + 1) / 2;
}

uint32_t leaf_node_left_split_count(const Table* table) {
  return (leaf_node_max_cells(table) + 1) - leaf_node_right_split_count(table);
}

void* leaf_node_cell(const Table* table, void* node, uint32_t cell_num) {
  return node + LEAF_NODE_HEADER_SIZE + cell_num * leaf_node_cell_size(table);
}

uint32_t* leaf_node_key(const Table* table, void* node, uint32_t cell_num) {
  return leaf_node_cell(table, node, cell_num);
}

void* leaf_node_value(const Table* table, void* node, uint32_t cell_num) {
  return leaf_node_cell(table, node, cell_num) + LEAF_NODE_KEY_SIZE;
}

void leaf_node_split_and_insert(Cursor* cursor, uint32_t key,
                                const void* value) {
  /*
  Create a new node and move half the cells over.
  Insert the new value in one of the two nodes.
  Update parent or create a new parent.
  */

  void* old_node = get_page(cursor->table->pager, cursor->page_num);
  uint32_t old_max = get_node_max_key(cursor->table, old_node);
  uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
  void* new_node = get_page(cursor->table->pager, new_page_num);
  uint32_t max_cells = leaf_node_max_cells(cursor->table);
  uint32_t left_split_count = leaf_node_left_split_count(cursor->table);
  uint32_t right_split_count = leaf_node_right_split_count(cursor->table);
  uint32_t cell_size = leaf_node_cell_size(cursor->table);

  initialize_leaf_node(new_node);
  *node_parent(new_node) = *node_parent(old_node);
  *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
  *leaf_node_next_leaf(old_node) = new_page_num;

  /*
  All existing keys plus new key should should be divided
  evenly between old (left) and new (right) nodes.
  Starting from the right, move each key to correct position.
  */
  for (uint32_t i = max_cells + 1; i > 0; i--) {
    uint32_t cell_num = i - 1;
    void* destination_node;
    if (cell_num >= left_split_count) {
      destination_node = new_node;
    } else {
      destination_node = old_node;
    }
    uint32_t index_within_node = cell_num % left_split_count;
    void* destination =
        leaf_node_cell(cursor->table, destination_node, index_within_node);

    if (cell_num == cursor->cell_num) {
      memcpy(leaf_node_value(cursor->table, destination_node, index_within_node),
             value, cursor->table->value_size);
      *leaf_node_key(cursor->table, destination_node, index_within_node) = key;
    } else if (cell_num > cursor->cell_num) {
      memcpy(destination, leaf_node_cell(cursor->table, old_node, cell_num - 1),
             cell_size);
    } else {
      memcpy(destination, leaf_node_cell(cursor->table, old_node, cell_num),
             cell_size);
    }
  }

  /* Update cell count on both leaf nodes */
  *(leaf_node_num_cells(old_node)) = left_split_count;
  *(leaf_node_num_cells(new_node)) = right_split_count;

  if (is_node_root(old_node)) {
    return create_new_root(cursor->table, new_page_num);
  } else {
    uint32_t parent_page_num = *node_parent(old_node);
    uint32_t new_max = get_node_max_key(cursor->table, old_node);
    void* parent = get_page(cursor->table->pager, parent_page_num);

    update_internal_node_key(parent, old_max, new_max);
    internal_node_insert(cursor->table, parent_page_num, new_page_num);
    return;
  }
}

void leaf_node_insert(Cursor* cursor, uint32_t key, const void* value) {
  void* node = get_page(cursor->table->pager, cursor->page_num);

  uint32_t num_cells = *leaf_node_num_cells(node);
  uint32_t max_cells = leaf_node_max_cells(cursor->table);
  uint32_t cell_size = leaf_node_cell_size(cursor->table);

  if (num_cells >= max_cells) {
    // Node full
    leaf_node_split_and_insert(cursor, key, value);
    return;
  }

  if (cursor->cell_num < num_cells) {
    // Make room for new cell
    for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
      memcpy(leaf_node_cell(cursor->table, node, i),
             leaf_node_cell(cursor->table, node, i - 1), cell_size);
    }
  }

  *(leaf_node_num_cells(node)) += 1;
  *(leaf_node_key(cursor->table, node, cursor->cell_num)) = key;
  memcpy(leaf_node_value(cursor->table, node, cursor->cell_num), value,
         cursor->table->value_size);
}

//
Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
  void* node = get_page(table->pager, page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  Cursor* cursor = malloc(sizeof(Cursor));
  cursor->table = table;
  cursor->page_num = page_num;
  cursor->end_of_table = false;

  // Binary search
  uint32_t min_index = 0;
  uint32_t one_past_max_index = num_cells;
  while (one_past_max_index != min_index) {
    uint32_t index = (min_index + one_past_max_index) / 2;
    uint32_t key_at_index = *leaf_node_key(table, node, index);
    if (key == key_at_index) {
      cursor->cell_num = index;
      return cursor;
    }
    if (key < key_at_index) {
      one_past_max_index = index;
    } else {
      min_index = index + 1;
    }
  }

  cursor->cell_num = min_index;
  return cursor;
}
