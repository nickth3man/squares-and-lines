CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c11
LDLIBS = -lcurl

# Windows (MSVC): see README.md for the cl.exe build command

gridscape: main.c
	$(CC) $(CFLAGS) -o gridscape main.c $(LDLIBS)

clean:
	rm -f gridscape

.PHONY: clean
