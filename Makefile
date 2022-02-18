CC = gcc
RM = rm
COPT = -O2
N_TESTS = 6

TESTS := $(shell ruby -e "puts (0..$(N_TESTS)).to_a.map{|x| 'test%02d.out' % x}.join ' '")

.PHONY: all clean
all: $(TESTS)
clean:
	$(RM) *.o *.out

test%.out: tests/test%.c zzcore.o
	$(CC) -o $@ $(COPT) $^

zzcore.o: zzcore.c zzcore.h
	$(CC) -c $(COPT) $^