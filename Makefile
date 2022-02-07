CC = gcc
RM = rm
COPT = -O1 -g0

.PHONY: all clean
all: test0.out
clean:
	$(RM) *.o *.out

test0.out: test0.c zzcore.c
	$(CC) -o $@ $(COPT) $<
