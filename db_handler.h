#ifndef DB_HANDLER_H
#define DB_HANDLER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern FILE* db_file;
extern int rec_size;
extern int num_recs;

int create_db(char* db_name,int num_records, int record_size);
int open_db(char* db_name);
int close_db(char* db_name);
void * read_db(char* db_name, int key);
int store_db(char* db_name, int key, void* record);
int update_db(char* db_name, int key, void* record);
int delete_db(char* db_name, int key);

#endif