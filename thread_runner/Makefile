CFLAGS=-O2

NUM_THREADS=4
BUF_SIZE=20m
OPTIONS=

thread_runner:
	gcc $(CFLAGS) thread_runner.c -lpthread -o thread_runner

clean:
	rm -f thread_runner

test:
	./thread_runner $(NUM_THREADS) $(BUF_SIZE) $(OPTIONS)
