CC=gcc
FLAGS=-Wall -pedantic -Wextra -pg
LDLIBS=
EXE=d

${EXE}: ed.c
	${CC} ${FLAGS} -o ${EXE} ed.c ${LDLIBS}
