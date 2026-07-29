#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "btree_leaf.h"
#include "btree_node.h"
#include "table.h"

Table* db_open_with_value_size(const char* filename, size_t value_size) {
  if (value_size == 0 ||
      value_size > LEAF_NODE_SPACE_FOR_CELLS - LEAF_NODE_KEY_SIZE) {
    printf("Invalid table value size.\n");
    exit(EXIT_FAILURE);
  }

  Pager* pager = pager_open(filename);

  Table* table = malloc(sizeof(Table));
  table->pager = pager;
  table->root_page_num = 0;
  table->value_size = (uint32_t)value_size;

  if (pager->num_pages == 0) {
    // New database file. Initialize page 0 as leaf node.
    void* root_node = get_page(pager, 0);
    initialize_leaf_node(root_node);
    set_node_root(root_node, true);
    *node_parent(root_node) = PAGER_NO_FREE_PAGE;
  } else {
    void* root_node = get_page(pager, 0);
    uint32_t free_page_head = *node_parent(root_node);
    if (free_page_head > 0 && free_page_head < pager->num_pages) {
      pager->free_page_head = free_page_head;
    } else {
      pager->free_page_head = PAGER_NO_FREE_PAGE;
      *node_parent(root_node) = PAGER_NO_FREE_PAGE;
    }
  }

  return table;
}

// Flush all pages and cleanly shut down the database.
void db_close(Table* table) {
  Pager* pager = table->pager;
  void* root_node = get_page(pager, table->root_page_num);
  *node_parent(root_node) = pager->free_page_head;

  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (pager->pages[i] == NULL) {
      continue;
    }
    pager_flush(pager, i);
    free(pager->pages[i]);
    pager->pages[i] = NULL;
  }

  int result = close(pager->file_descriptor);
  if (result == -1) {
    printf("Error closing db file.\n");
    exit(EXIT_FAILURE);
  }
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    void* page = pager->pages[i];
    if (page) {
      free(page);
      pager->pages[i] = NULL;
    }
  }
  free(pager);
  free(table);
}
