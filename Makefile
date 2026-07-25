
LIBS  += -lz -lbluetooth
CFLAGS = -Wall
CC ?= gcc

SRC=$(wildcard *.c)

nrf_dfu: $(SRC)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

