
CFLAGS = -I.

SRC= \
	config/config.c \
	dev-mgr/dev-manager.c \
	dev-mgr/hash.c \
	log/log.c \
	mm/pool.c \
	mm/dbg_malloc.c

OBJ=${SRC:.c=.o}
TARGET=liblvm.a

all: ${TARGET}

liblvm.a: $(OBJ)
	rm -f $@
	ar r $@ $(OBJ)
	ranlib $@

clean:
	rm -f ${OBJ} ${TARGET}
