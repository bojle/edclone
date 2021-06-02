CC=gcc
FLAGS=-Wall -pedantic -Wextra -g
LDLIBS=
EXE=d

${EXE}: ed.c
	${CC} ${FLAGS} -o ${EXE} ed.c ${LDLIBS}
