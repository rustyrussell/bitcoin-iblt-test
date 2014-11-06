VERSION:=$(shell git describe --dirty --always 2>/dev/null || echo Unknown)
OPT:= -g3 -O3 -flto
#OPT:= -g3
CFLAGS = $(OPT) -Wall -Wundef -Wmissing-prototypes -Wmissing-declarations -Wstrict-prototypes -Wold-style-definition -fstack-protector -I$(CCANDIR) -DVERSION=\"$(VERSION)\"
LDLIBS := -lcrypto
LDFLAGS := $(OPT)

CCAN_OBJS := ccan-tal.o ccan-take.o ccan-list.o ccan-str.o ccan-opt-helpers.o ccan-opt.o ccan-opt-parse.o ccan-opt-usage.o ccan-noerr.o ccan-hash.o ccan-err.o ccan-invbloom.o
CCANDIR=../ccan/

test-iblt: test-iblt.o $(CCAN_OBJS)

clean:
	rm -f $(CCAN_OBJS) test-iblt.o

ccan-tal.o: $(CCANDIR)/ccan/tal/tal.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-take.o: $(CCANDIR)/ccan/take/take.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-list.o: $(CCANDIR)/ccan/list/list.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-str.o: $(CCANDIR)/ccan/str/str.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-opt.o: $(CCANDIR)/ccan/opt/opt.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-opt-helpers.o: $(CCANDIR)/ccan/opt/helpers.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-opt-parse.o: $(CCANDIR)/ccan/opt/parse.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-opt-usage.o: $(CCANDIR)/ccan/opt/usage.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-noerr.o: $(CCANDIR)/ccan/noerr/noerr.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-hash.o: $(CCANDIR)/ccan/hash/hash.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-err.o: $(CCANDIR)/ccan/err/err.c
	$(CC) $(CFLAGS) -c -o $@ $<
ccan-invbloom.o: $(CCANDIR)/ccan/invbloom/invbloom.c
	$(CC) $(CFLAGS) -c -o $@ $<
