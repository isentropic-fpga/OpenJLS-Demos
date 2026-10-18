# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
# Makefile: ojls_server (board) and ojls_client (host); cross with CROSS_COMPILE=

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Icommon -Isrc

SERVER_SRC := src/ojls_server.c src/uio.c src/udmabuf.c src/axidma.c
CLIENT_SRC := host/ojls_client.c

all: ojls_server ojls_client

ojls_server: $(SERVER_SRC) src/uio.h src/udmabuf.h src/axidma.h src/ojls_regs.h common/ojls_proto.h
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC)

ojls_client: $(CLIENT_SRC) common/ojls_proto.h
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRC)

clean:
	rm -f ojls_server ojls_client

.PHONY: all clean
