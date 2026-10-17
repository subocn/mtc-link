# 作者：苏勃(subo)
# 令牌套件 0x00 / 0x01，以及厂商套件外壳
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Iinclude -I../chg_link/include

.PHONY: all test clean size

all: test

test: test_host
	./test_host

test_host: src/chg_sha256.c src/chg_crypt.c tests/test_chg_crypt.c
	$(CC) $(CFLAGS) -o $@ $^

size:
	$(CC) -Os -std=c11 -Wall -Wextra -Werror -fstack-usage -Iinclude -I../chg_link/include \
		-c src/chg_sha256.c -o chg_sha256.o
	$(CC) -Os -std=c11 -Wall -Wextra -Werror -fstack-usage -Iinclude -I../chg_link/include \
		-c src/chg_crypt.c -o chg_crypt.o
	size chg_sha256.o chg_crypt.o
	@echo "--- stack ---"
	@cat chg_sha256.su chg_crypt.su

clean:
	rm -f test_host *.o *.su
