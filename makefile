all:prog

prog:main.o
	gcc main.o -o prog -lrt

main.o:main.c
	gcc -c main.c -o main.o -std=c99
main.c:
