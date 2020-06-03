
lps: main.c lps.h
	gcc -Wall -O3 -o $@ $< -lpthread
