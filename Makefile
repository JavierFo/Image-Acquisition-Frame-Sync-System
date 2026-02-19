INCLUDE_DIRS = 
LIB_DIRS = 
CC=gcc

CDEFS=
CFLAGS= -lrt -lm -O3 -D_GNU_SOURCE $(INCLUDE_DIRS) $(CDEFS)
LIBS= 

HFILES= 
CFILES= rt_refactored.c rt_refactored_10hz.c rt_multiprocess.c rt_multicore.c rt_mmapped.c rt_sockets.c

SRCS= ${HFILES} ${CFILES}
OBJS= ${CFILES:.c=.o}

all:	rt_refactored rt_refactored_10hz rt_multiprocess rt_multicore rt_mmapped rt_sockets

clean:
	-rm -f *.o *.d frames/*.pgm frames/*.ppm
	-rm -f rt_refactored rt_refactored_10hz rt_multiprocess rt_multicore rt_mmapped rt_sockets

rt_REFACTORED: rt_refactored.o
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $@.o -lpthread -lm -O3
	
rt_refactored_10hz: rt_refactored_10hz.o
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $@.o -lpthread -lm -O3
	
rt_multicore: rt_multicore.o
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $@.o -lpthread -lm -O3
	
rt_multiprocess: rt_multiprocess.o
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $@.o -lpthread -lm -O3
	
rt_mmapped: rt_mmapped.o
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $@.o -lpthread -lm -O3
	
rt_sockets: rt_sockets.o
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $@.o -lpthread -lm -O3

depend:

.c.o:
	$(CC) $(CFLAGS) -c $<
