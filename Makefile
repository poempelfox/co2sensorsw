# $Id: Makefile $
# Makefile for Foxtemp2016

CC	= gcc

SRCS	= co2sensord.c
PROG	= co2sensord

# compiler flags
CFLAGS	= -O2 -Wall -Wno-pointer-sign -std=gnu11 -DBRAINDEADOS
LDFALGS = 

OBJS	= $(SRCS:.c=.o)

all: $(PROG)

compile: $(OBJS)
	$(CC) -o $(PROG) $(CFLAGS) $(LDFLAGS) $(OBJS)

%o : %c 
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(PROG) $(OBJS) *~

