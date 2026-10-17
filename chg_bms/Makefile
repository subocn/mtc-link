# 作者：苏勃(subo)
# BMS 侧充电通信
# 作者：苏勃(subo)
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Iinclude -I../chg_link/include

.PHONY: all test clean

all: test

test: test_host
	./test_host

test_host: src/chg_bms.c tests/test_chg_bms.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_host
