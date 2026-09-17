CXX ?= g++
CXXFLAGS ?= -std=c++11 -I../include -g -Wall

.PHONY: all check clean

all: recv_semantics_test

recv_semantics_test: recv_semantics_test.cc
	$(CXX) $(CXXFLAGS) $< -o $@ -lpthread

check: recv_semantics_test
	./recv_semantics_test

clean:
	rm -f recv_semantics_test
