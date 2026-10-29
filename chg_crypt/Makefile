# 作者：苏勃(subo)
# SPDX-License-Identifier: Apache-2.0
# 令牌套件 0x00 / 0x01，以及厂商套件外壳
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Iinclude -I../chg_link/include

.PHONY: all test clean size

all: test

test: test_host test_omit test_ext chg_crypt.o
	./test_host
	./test_omit
	./test_ext
	@nm test_omit | grep -q chg_hmac && { echo "omit image still has hmac"; exit 1; } || true
	@nm chg_crypt.o | grep -E 'chg_hmac|chg_sha256' && { echo "session object still names hmac"; exit 1; } || true

test_host: src/chg_sha256.c src/chg_crypt.c tests/test_chg_crypt.c
	$(CC) $(CFLAGS) -o $@ $^

test_omit: src/chg_crypt.c tests/test_omit.c
	$(CC) $(CFLAGS) -DCHG_CRYPT_OMIT_HMAC=1 -o $@ $^

test_ext: src/chg_crypt.c src/chg_sha256.c tests/test_ext.c \
	../chg_bms/src/chg_bms.c ../chg_charger/src/chg_charger.c
	$(CC) $(CFLAGS) -I../chg_bms/include -I../chg_charger/include -o $@ $^

chg_crypt.o: src/chg_crypt.c
	$(CC) $(CFLAGS) -c -o $@ src/chg_crypt.c

size:
	$(CC) -Os -std=c11 -Wall -Wextra -Werror -fstack-usage -Iinclude -I../chg_link/include \
		-c src/chg_sha256.c -o chg_sha256.o
	$(CC) -Os -std=c11 -Wall -Wextra -Werror -fstack-usage -Iinclude -I../chg_link/include \
		-c src/chg_crypt.c -o chg_crypt.o
	$(CC) -Os -std=c11 -Wall -Wextra -Werror -Iinclude -I../chg_link/include \
		-DCHG_CRYPT_OMIT_HMAC=1 -c src/chg_crypt.c -o chg_crypt_omit.o
	size chg_sha256.o chg_crypt.o chg_crypt_omit.o
	@echo "--- stack ---"
	@cat chg_sha256.su chg_crypt.su

clean:
	rm -f test_host test_omit test_ext *.o *.su
