#
# WARNING
#
# This is a temporary Makefile. You need to edit the IPATH/LPATH variables to
# point to build-dir of LVM2. You may then just run "make" to build the lvmetad
# binary and the test client.
#

SHARED = ../common/daemon-shared.c
CLIENT = ../common/daemon-client.c $(SHARED)
SERVER = ../common/daemon-server.c $(SHARED)
SHARED_H = ../common/daemon-shared.h
CLIENT_H = ../common/daemon-client.h $(SHARED_H)
SERVER_H = ../common/daemon-server.h $(SHARED_H)

LIBS = -ldevmapper -lpthread -ldevmapper-event -llvm-internal -ldl
IPATH = -I../common -I/srv/build/lvm2/cvs-lvmetad/default/include
BUILDDIR = /srv/build/lvm2/cvs-lvmetad/default
LPATH = -L$(BUILDDIR)/libdm -L$(BUILDDIR)/lib -L$(BUILDDIR)/daemons/dmeventd

all: testclient lvmetad

testclient: testclient.c $(CLIENT_H) $(CLIENT)
	gcc -g testclient.c $(CLIENT) $(IPATH) $(LPATH) $(LIBS) -o testclient

lvmetad: lvmetad-core.c ../common/daemon-server.c ../common/daemon-server.h ../common/daemon-shared.h ../common/daemon-shared.c
	gcc -g lvmetad-core.c $(SERVER) $(IPATH) $(LPATH) $(LIBS) -o lvmetad

check: testclient lvmetad
	./test.sh "$(BUILDDIR)/libdm:$(BUILDDIR)/daemons/dmeventd"

clean:
	rm -f testclient lvmetad
