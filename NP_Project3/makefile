CC = g++
CXXFLAGS = -Wall -O2 -std=c++17 -pthread

.PHONY: all clean part1 part2

all: part1 part2

part1: http_server console.cgi

http_server: http_server.cpp
	$(CC) http_server.cpp -o http_server $(CXXFLAGS) -lboost_system
console.cgi: console.cpp
	$(CC) console.cpp -o console.cgi $(CXXFLAGS) -lboost_system

part2: cgi_server

cgi_server: cgi_server.cpp
	$(CC) cgi_server.cpp -o cgi_server -lws2_32 -lwsock32 $(CXXFLAGS) -mconsole 
#-IC:/MinGW/include

clean:
	rm -f http_server console.cgi cgi_server