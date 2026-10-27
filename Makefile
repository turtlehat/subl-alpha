CFLAGS = -O2 -Wall -fPIC
LDLIBS = -ldl -lGL

shim.so: shim.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

clean:
	rm -f shim.so

.PHONY: clean
