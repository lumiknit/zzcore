CC = gcc
RM = rm
COPT = -O0 -g

TESTS = \
	test00.out \
	test01.out \
	test02.out

.PHONY: all clean
all: $(TESTS)
clean:
	$(RM) *.o *.out

test%.out: tests/test%.c zzcore.c
	$(CC) -o $@ $(COPT) $^
