all:prog

prog:main.c
	gcc main.c -o prog -lrt -std=c99 -pthread
main.c:
