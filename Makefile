CC = gcc
CFLAGS = -lncursesw

run: presentation.o
	./presentation.o

%.o: %.c
	$(CC) $< $(CFLAGS) -o$@

clean:
	rm *.o
