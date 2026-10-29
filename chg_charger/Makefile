# 作者：苏勃(subo)
# SPDX-License-Identifier: Apache-2.0
# 充电机侧充电通信
# 作者：苏勃(subo)
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Iinclude -I../chg_link/include

.PHONY: all test clean

all: test

test: test_host
	./test_host

test_host: src/chg_charger.c tests/test_chg_charger.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_host
