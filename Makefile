CFLAGS  = `pkg-config --cflags libnetconf2 libyang`
LIBS    = `pkg-config --libs libnetconf2 libyang`

CFLAGS += -std=c++11 -Wall
LIBS += -lpthread

OBJS += main.o
OBJS += rpc_callbacks.o
OBJS += auth_callbacks.o

main : ${OBJS}
	g++ $^ -o $@ ${LIBS}

%.c : %.o
	g++ -c $< -o $@ ${CFLAGS}

