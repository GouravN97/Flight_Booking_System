#ifndef BTREE_NODE_H
#define BTREE_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include "table.h"

#define NODE_TYPE_SIZE ((uint32_t)sizeof(uint8_t))
#define NODE_TYPE_OFFSET 0
#define IS_ROOT_SIZE ((uint32_t)sizeof(uint8_t))
#define IS_ROOT_OFFSET (NODE_TYPE_OFFSET + NODE_TYPE_SIZE)
#define PARENT_POINTER_SIZE ((uint32_t)sizeof(uint32_t))
#define PARENT_POINTER_OFFSET (IS_ROOT_OFFSET + IS_ROOT_SIZE)
#define COMMON_NODE_HEADER_SIZE \
  (NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE)
#define INTERNAL_NODE_NUM_KEYS_SIZE ((uint32_t)sizeof(uint32_t))
#define INTERNAL_NODE_NUM_KEYS_OFFSET COMMON_NODE_HEADER_SIZE
#define INTERNAL_NODE_RIGHT_CHILD_SIZE ((uint32_t)sizeof(uint32_t))
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET \
  (INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE)

typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

NodeType get_node_type(void* node);
bool is_node_root(void* node);
void set_node_root(void* node, bool is_root);
void set_node_type(void* node, NodeType type);
uint32_t* node_parent(void* node);
uint32_t get_node_max_key(Table* table, void* node);

#endif
