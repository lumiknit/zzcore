CC = gcc
RM = rm
COPT = -O0 -g
N_TESTS = 3

TESTS := $(shell ruby -e "puts (0..$(N_TESTS)).to_a.map{|x| 'test%02d.out' % x}.join ' '")

.PHONY: all clean
all: $(TESTS)
clean:
	$(RM) *.o *.out

test%.out: tests/test%.c zzcore.c
	$(CC) -o $@ $(COPT) $^
