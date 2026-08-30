CC = gcc
CFLAGS = -Wall -Wextra -pthread -std=gnu11
CPPFLAGS = -Iserver -Iclient -Istorage

MY_DBMS_DIR = my_dbms
MY_DBMS_CPPFLAGS = -I$(MY_DBMS_DIR)/src/api -I$(MY_DBMS_DIR)/src/storage \
                   -I$(MY_DBMS_DIR)/src/btree
MY_DBMS_SRCS = $(MY_DBMS_DIR)/src/api/db.c \
               $(MY_DBMS_DIR)/src/storage/table.c \
               $(MY_DBMS_DIR)/src/storage/pager.c \
               $(MY_DBMS_DIR)/src/storage/cursor.c \
               $(MY_DBMS_DIR)/src/btree/btree_node.c \
               $(MY_DBMS_DIR)/src/btree/btree_leaf.c \
               $(MY_DBMS_DIR)/src/btree/btree_internal.c \
               $(MY_DBMS_DIR)/src/btree/btree_algos.c

SERVER_SRCS = apps/server_app.c \
               server/server_main.c server/server_state.c server/server_protocol.c \
               server/auth_service.c server/flight_service.c server/booking_service.c \
               server/waitlist_service.c server/password.c server/token.c \
               server/admin_ipc.c storage/storage_service.c storage/db_handler.c \
               $(MY_DBMS_SRCS)
CLIENT_SRCS = apps/client_app.c client/client.c
ADMIN_SRCS = apps/admin_app.c client/client.c

.PHONY: all clean server client admin reset-bookings-db test-concurrency concurrent_booking_test api

all: server_app client_app admin_app

concurrent_booking_test: tests/concurrent_booking_test.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

test-concurrency: concurrent_booking_test server_app
	./tests/run_concurrency_tests.sh

server_app: $(SERVER_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(MY_DBMS_CPPFLAGS) -o $@ $^

client_app: $(CLIENT_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

admin_app: $(ADMIN_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

server: server_app
client: client_app
admin: admin_app

clean:
	rm -f server_app client_app admin_app concurrent_booking_test *.db auth.secret

reset-bookings-db:
	rm -f bookings.db

api:
	python3 web/api/gateway.py
