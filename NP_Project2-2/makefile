CC = g++
CXXFLAGS = -Wall -O2 -std=c++17

NP_MULTI_PROC_SRC = np_multi_proc.cpp
NP_MULTI_PROC_OBJ = np_multi_proc

# 找到 src/ 內的所有 .cpp 檔案
SRC_FILES = $(wildcard src/*.cpp)
BIN_FILES = $(patsubst src/%.cpp,bin/%,$(SRC_FILES))

# 內建指令
BUILD_IN_CMD = ls cat wc
BIN_CMD_FILES = $(patsubst %,bin/%,$(BUILD_IN_CMD))

.PHONY: all clean

all: bin user_pipe $(NP_MULTI_PROC_OBJ) $(BIN_FILES) $(BIN_CMD_FILES)

# 建立 bin/ 目錄
bin:
	mkdir -p bin

user_pipe:
	mkdir -p user_pipe
	chmod 755 user_pipe

$(NP_MULTI_PROC_OBJ): $(NP_MULTI_PROC_SRC)
	$(CC) $(CXXFLAGS) $< -g -o $@

# 編譯 src/*.cpp 到 bin/
bin/%: src/%.cpp | bin
	$(CC) $(CXXFLAGS) $< -o $@

# 複製 /bin/ 內建指令到 bin/
bin/%: bin
	cp -f $(shell which $(@F)) $@

clean:
	rm -rf bin user_pipe $(NP_MULTI_PROC_OBJ)
