CC=gcc

nodelay_allping: allping.c
	$(CC) -DSEND_DELAY=0.0  -o allping  allping.c

delay_allping: allping.c
	$(CC) -DSEND_DELAY=0.1  -o allping  allping.c

clean:
	rm *.o allping iplist
