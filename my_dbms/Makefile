CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -g
CPPFLAGS ?= -Isrc/api -Isrc/storage -Isrc/btree

SRCS = src/api/db.c \
       src/storage/pager.c src/storage/table.c src/storage/cursor.c \
       src/btree/btree_node.c src/btree/btree_leaf.c src/btree/btree_internal.c \
       src/btree/btree_algos.c
OBJS = $(SRCS:.c=.o)
HDRS = src/api/db.h \
       src/storage/pager.h src/storage/table.h src/storage/cursor.h \
       src/btree/btree_node.h src/btree/btree_leaf.h src/btree/btree_internal.h \
       src/btree/btree_algos.h

TARGET = libmy_dbms.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	ar rcs $(TARGET) $(OBJS)

%.o: %.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
