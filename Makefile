
LIBS  = -lz -lbluetooth
CFLAGS = -Wall

# Should be equivalent to your list of C files, if you don't build selectively
SRC=$(wildcard *.c)

nrf_dfu: $(SRC)
	gcc -o $@ $^ $(CFLAGS) $(LIBS)

