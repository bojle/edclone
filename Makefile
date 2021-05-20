CC=gcc
FLAGS=-Wall -pedantic -Wextra -g
LDLIBS=
src=$(wildcard *.c)
obj=$(src:*.c *.o)
EXE=

a: a.c
	${CC} ${FLAGS} -o a a.c ${LDLIBS}

d: d.c
	${CC} ${FLAGS} -o d d.c ${LDLIBS}

${EXE}: main.o ll.o io.o parse.o ed.h 
	${CC} ${FLAGS} -o ${EXE} main.o ll.o io.o parse.o ${LDLIBS}

main.o: main.c ed.h
	${CC} ${FLAGS} -c main.c

ll.o: ll.c ed.h
	${CC} ${FLAGS} -c ll.c

io.o: io.c ed.h
	${CC} ${FLAGS} -c io.c

parse.o: parse.c ed.h
	${CC} ${FLAGS} -c parse.c
