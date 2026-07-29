#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "btree_internal.h"
#include "btree_leaf.h"
#include "btree_node.h"

NodeType get_node_type(void* node) {
  uint8_t value = *((uint8_t*)(node + NODE_TYPE_OFFSET));
  return (NodeType)value;
}

bool is_node_root(void* node) {
  uint8_t value = *((uint8_t*)(node + IS_ROOT_OFFSET));
  return (bool)value;
}

void set_node_root(void* node, bool is_root) {
  uint8_t value = is_root;
  *((uint8_t*)(node + IS_ROOT_OFFSET)) = value;
}

void set_node_type(void* node, NodeType type) {
  uint8_t value = type;
  *((uint8_t*)(node + NODE_TYPE_OFFSET)) = value;
}

uint32_t* node_parent(void* node) { return node + PARENT_POINTER_OFFSET; }

uint32_t get_node_max_key(Table* table, void* node) {
  if (get_node_type(node) == NODE_LEAF) {
    return *leaf_node_key(table, node, *leaf_node_num_cells(node) - 1);
  }
  void* right_child = get_page(table->pager, *internal_node_right_child(node));
  return get_node_max_key(table, right_child);
}
