#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "db_handler.h"
#include <pthread.h>
#include <semaphore.h>
    
FILE* db_file = NULL;
int rec_size = 0;
int num_recs = 0;

int create_db(char* db_name, int num_records, int record_size) {
    

    FILE *db_file_local = fopen(db_name, "wb");
    if (db_file_local == NULL) {
        perror("Error creating database");
        return -1;
    }

    num_recs = num_records;
    rec_size = record_size;

    fwrite(&num_records, sizeof(int), 1, db_file_local);
    fwrite(&record_size, sizeof(int), 1, db_file_local);
    fclose(db_file_local);

    return 0;
}

int open_db(char* db_name) {

    db_file = fopen(db_name, "rb+");
    if (db_file == NULL) {
        perror("Error opening database");
        return -1;
    }

    fread(&num_recs, sizeof(int), 1, db_file);
    fread(&rec_size, sizeof(int), 1, db_file);

    return 0;   
}

int store_db(char* db_name, int key, void* record) {
    (void)db_name;

    if (db_file == NULL) {
        perror("Database not opened");
        return -1;
    }

    fseek(db_file, sizeof(int)*2 + key * rec_size, SEEK_SET);
    fwrite(record, rec_size, 1, db_file);

    return 0;
}

void* read_db(char* db_name, int key) {
    (void)db_name;

    if (db_file == NULL) {
        perror("Database not opened");
        return NULL;
    }

    fseek(db_file, sizeof(int)*2 + key * rec_size, SEEK_SET);

    void* record = malloc(rec_size);
    fread(record, rec_size, 1, db_file);

    return record;
}

int update_db(char *db_name, int key, void *record) {
    (void)db_name;

    if (db_file == NULL) {
        perror("Database not opened");
        return -1;
    }

    fseek(db_file, sizeof(int)*2 + key * rec_size, SEEK_SET);
    fwrite(record, rec_size, 1, db_file);

    return 0;
}

int delete_db(char* db_name, int key) {
    (void)db_name;

    if (db_file == NULL) {
        perror("Database not opened");
        return -1;
    }

    fseek(db_file, sizeof(int)*2 + key * rec_size, SEEK_SET);

    char* empty_record = calloc(1, rec_size);
    fwrite(empty_record, rec_size, 1, db_file);
    free(empty_record);

    return 0;
}

int close_db(char* db_name) {
    (void)db_name;

    if (db_file == NULL) {
        perror("Database not opened");
        return -1;
    }

    fclose(db_file);
    db_file = NULL;

    return 0;
}