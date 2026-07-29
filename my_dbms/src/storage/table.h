#ifndef TABLE_H
#define TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "pager.h"

typedef struct {
  Pager* pager;
  uint32_t root_page_num;
  uint32_t value_size;
} Table;

Table* db_open_with_value_size(const char* filename, size_t value_size);
void db_close(Table* table);

#endif
