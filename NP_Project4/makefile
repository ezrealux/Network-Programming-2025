TARGET := socks_server pj5.cgi

CXX := g++
ifeq (/usr/bin/g++-11,$(wildcard /usr/bin/g++-11*))
    CXX=g++-11
endif

CXXFLAGS := -std=c++17 -Wall -Wextra -g

.PHONY: all
all: $(TARGET)

socks_server: socks_server.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ -lpthread
pj5.cgi: console.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ -lpthread

.PHONY: clean
clean:
	rm -f *.o $(TARGET)