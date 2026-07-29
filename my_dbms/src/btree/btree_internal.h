#ifndef BTREE_INTERNAL_H
#define BTREE_INTERNAL_H

#include <stdint.h>

#include "btree_node.h"
#include "cursor.h"
#include "table.h"

#define INVALID_PAGE_NUM UINT32_MAX

#define INTERNAL_NODE_KEY_SIZE ((uint32_t)sizeof(uint32_t))
#define INTERNAL_NODE_CHILD_SIZE ((uint32_t)sizeof(uint32_t))
#define INTERNAL_NODE_CELL_SIZE \
  (INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE)
#define INTERNAL_NODE_HEADER_SIZE \
  (COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + \
   INTERNAL_NODE_RIGHT_CHILD_SIZE)
#define INTERNAL_NODE_MAX_KEYS 3

uint32_t* internal_node_num_keys(void* node);
uint32_t* internal_node_right_child(void* node);
uint32_t* internal_node_cell(void* node, uint32_t cell_num);
uint32_t* internal_node_child(void* node, uint32_t child_num);
uint32_t* internal_node_key(void* node, uint32_t key_num);
uint32_t internal_node_find_child(void* node, uint32_t key);
void initialize_internal_node(void* node);
void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key);
void internal_node_split_and_insert(Table* table, uint32_t parent_page_num,
                                    uint32_t child_page_num);
void internal_node_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num);
Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key);

#endif
