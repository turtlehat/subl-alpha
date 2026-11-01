CFLAGS = -O2 -Wall -fPIC
LDLIBS = -ldl -lGL

shim.so: shim.c shim-linux.c shim.h
	$(CC) $(CFLAGS) -shared -o $@ shim.c shim-linux.c $(LDLIBS)

clean:
	rm -f shim.so

.PHONY: clean
