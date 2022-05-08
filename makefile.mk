all: peer ds
# make rule per il peer
peer: peer.o
	gcc peer.o -o peer

ds: ds.o
	gcc ds.o -o ds

peer.o: peer.c
	gcc -Wall -c peer.c
# make rule per il ds
ds.o: ds.c
	gcc -Wall -c ds.c
# pulizia dei file della compilazione (eseguito con ‘make clean’ da terminale)
clean:
	rm *.o
