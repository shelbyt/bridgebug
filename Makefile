CC=gcc

allping: allping.o
	$(CC) -o allping allping.o

all: allping

clean:
	rm *.o allping
