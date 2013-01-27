CFLAGS=-g -Wall -O3
CXXFLAGS=$(CFLAGS)
OBJECTS=main.o dywapitchtrack.o
LIBS=-lasound -lncurses

pitch-hero: $(OBJECTS)
	g++ -o $@ $^ $(LIBS)

clean:
	rm -f pitch-hero $(OBJECTS)
