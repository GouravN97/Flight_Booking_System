#ifndef BTREE_LEAF_H
#define BTREE_LEAF_H

#include <stdint.h>

#include "btree_node.h"
#include "cursor.h"
#include "table.h"

#define LEAF_NODE_NUM_CELLS_SIZE ((uint32_t)sizeof(uint32_t))
#define LEAF_NODE_NUM_CELLS_OFFSET COMMON_NODE_HEADER_SIZE
#define LEAF_NODE_NEXT_LEAF_SIZE ((uint32_t)sizeof(uint32_t))
#define LEAF_NODE_NEXT_LEAF_OFFSET \
  (LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE)
#define LEAF_NODE_HEADER_SIZE \
  (COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE + LEAF_NODE_NEXT_LEAF_SIZE)
#define LEAF_NODE_KEY_SIZE ((uint32_t)sizeof(uint32_t))
#define LEAF_NODE_KEY_OFFSET 0
#define LEAF_NODE_VALUE_OFFSET (LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE)
#define LEAF_NODE_SPACE_FOR_CELLS (PAGE_SIZE - LEAF_NODE_HEADER_SIZE)

void initialize_leaf_node(void* node);
uint32_t* leaf_node_num_cells(void* node);
uint32_t* leaf_node_next_leaf(void* node);
uint32_t leaf_node_cell_size_for_value_size(uint32_t value_size);
uint32_t leaf_node_cell_size(const Table* table);
uint32_t leaf_node_max_cells(const Table* table);
uint32_t leaf_node_right_split_count(const Table* table);
uint32_t leaf_node_left_split_count(const Table* table);
void* leaf_node_cell(const Table* table, void* node, uint32_t cell_num);
uint32_t* leaf_node_key(const Table* table, void* node, uint32_t cell_num);
void* leaf_node_value(const Table* table, void* node, uint32_t cell_num);
void leaf_node_split_and_insert(Cursor* cursor, uint32_t key,
                                const void* value);
void leaf_node_insert(Cursor* cursor, uint32_t key, const void* value);
Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key);

#endif
