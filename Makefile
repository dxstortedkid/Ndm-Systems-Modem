CXX      = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -pedantic

modem: modem.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f modem

.PHONY: clean

