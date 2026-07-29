#ifndef PAGER_H
#define PAGER_H

#include <stdint.h>

#define PAGE_SIZE 4096
#define TABLE_MAX_PAGES 400
#define PAGER_NO_FREE_PAGE UINT32_MAX

typedef struct {
  int file_descriptor;
  uint32_t file_length;
  uint32_t num_pages;
  uint32_t free_page_head;
  void* pages[TABLE_MAX_PAGES];
} Pager;

void* get_page(Pager* pager, uint32_t page_num);
Pager* pager_open(const char* filename);
void pager_flush(Pager* pager, uint32_t page_num);
void pager_free_page(Pager* pager, uint32_t page_num);
uint32_t get_unused_page_num(Pager* pager);

#endif
