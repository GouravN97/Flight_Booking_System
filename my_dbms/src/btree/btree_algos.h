#ifndef BTREE_ALGOS_H
#define BTREE_ALGOS_H

#include <stdint.h>

#include "cursor.h"
#include "table.h"

Cursor* table_find(Table* table, uint32_t key);
void create_new_root(Table* table, uint32_t right_child_page_num);

#endif
