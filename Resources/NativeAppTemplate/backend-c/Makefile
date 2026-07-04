CC ?= cc
OUTPUT ?= backend
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -static

all:
	$(CC) $(CFLAGS) -o $(OUTPUT) main.c $(LDFLAGS)

clean:
	rm -f $(OUTPUT)
