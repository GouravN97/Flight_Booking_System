#ifndef DB_API_H
#define DB_API_H

#include <stdint.h>

#include "table.h"

typedef Table Db;

typedef enum {
  DB_OK,
  DB_NOT_FOUND,
  DB_DUPLICATE_KEY,
  DB_INVALID_ARGUMENT,
  DB_UNSUPPORTED_OPERATION,
} DbResult;

DbResult create(Db* db, uint32_t key, const void* value);
DbResult get(Db* db, uint32_t key, void* out_value);
DbResult update(Db* db, uint32_t key, const void* value);
DbResult delete(Db* db, uint32_t key);
const char* db_result_message(DbResult result);

#endif
