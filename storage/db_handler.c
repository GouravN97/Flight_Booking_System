#include "db_handler.h"

#include "btree_algos.h"
#include "btree_leaf.h"
#include "cursor.h"
#include "db.h"
#include "pager.h"
#include "table.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *db_file = NULL;
int rec_size = 0;
int num_recs = 0;

static Db *g_table = NULL;

static int upsert_record(uint32_t key, void *record) {
    DbResult result = update(g_table, key, record);
    if (result == DB_NOT_FOUND) {
        result = create(g_table, key, record);
    }
    return result == DB_OK ? 0 : -1;
}

int create_db(char *db_name, int num_records, int record_size) {
    num_recs = num_records;
    rec_size = record_size;

    Db *table = db_open_with_value_size(db_name, (size_t)record_size);
    if (table == NULL) {
        return -1;
    }

    db_close(table);
    return 0;
}

int open_db(char *db_name, int record_size) {
    (void)db_name;

    if (g_table != NULL) {
        fprintf(stderr, "Database already opened\n");
        return -1;
    }

    rec_size = record_size;
    g_table = db_open_with_value_size(db_name, (size_t)record_size);
    if (g_table == NULL) {
        return -1;
    }

    db_file = (FILE *)1;
    return 0;
}

int close_db(char *db_name) {
    (void)db_name;

    if (g_table == NULL) {
        fprintf(stderr, "Database not opened\n");
        return -1;
    }

    db_close(g_table);
    g_table = NULL;
    db_file = NULL;
    return 0;
}

void *read_db(char *db_name, int key) {
    (void)db_name;

    if (g_table == NULL) {
        fprintf(stderr, "Database not opened\n");
        return NULL;
    }

    void *record = malloc((size_t)rec_size);
    if (record == NULL) {
        return NULL;
    }

    DbResult result = get(g_table, (uint32_t)key, record);
    if (result == DB_NOT_FOUND) {
        memset(record, 0, (size_t)rec_size);
    } else if (result != DB_OK) {
        free(record);
        return NULL;
    }

    return record;
}

int store_db(char *db_name, int key, void *record) {
    (void)db_name;

    if (g_table == NULL) {
        fprintf(stderr, "Database not opened\n");
        return -1;
    }

    return upsert_record((uint32_t)key, record);
}

int update_db(char *db_name, int key, void *record) {
    return store_db(db_name, key, record);
}

int delete_db(char *db_name, int key) {
    (void)db_name;

    if (g_table == NULL) {
        fprintf(stderr, "Database not opened\n");
        return -1;
    }

    DbResult result = delete(g_table, (uint32_t)key);
    if (result == DB_NOT_FOUND) {
        return 0;
    }
    return result == DB_OK ? 0 : -1;
}

int foreach_record(DbRecordCallback callback, void *ctx) {
    if (g_table == NULL || callback == NULL) {
        return -1;
    }

    Cursor *cursor = table_start(g_table);
    while (!cursor->end_of_table) {
        void *node = get_page(g_table->pager, cursor->page_num);
        uint32_t key = *leaf_node_key(g_table, node, cursor->cell_num);
        int rc = callback(key, cursor_value(cursor), ctx);
        if (rc != 0) {
            free(cursor);
            return rc;
        }
        cursor_advance(cursor);
    }

    free(cursor);
    return 0;
}
