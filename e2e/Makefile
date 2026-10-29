# 作者：苏勃(subo)
# SPDX-License-Identifier: Apache-2.0
# e2e：背靠背 harness（chg_bms ↔ chg_charger）
# 作者：苏勃(subo)
#
# 编译 harness.c + chg_bms.c + chg_charger.c，进同一进程互相喂字节 / CAN。
# 任一端口（CHG_PORT_SIF / CAN / 485）任一阶段失败返回非零。
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror \
          -I../chg_bms/include \
          -I../chg_charger/include \
          -I../chg_link/include
LDFLAGS ?=

.PHONY: all test clean

all: test

test: e2e_host
	./e2e_host

e2e_host: harness.c ../chg_bms/src/chg_bms.c ../chg_charger/src/chg_charger.c ../chg_link/src/chg_battery_sim.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f e2e_host