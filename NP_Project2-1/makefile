CC = g++
CXXFLAGS = -Wall -O2 -std=c++17
# 設定 np_simple 和 np_simple_proc和 np_simple_proc
NP_SIMPLE_SRC = np_simple.cpp
NP_SIMPLE_OBJ = np_simple
NP_SIMPLE_PROC_SRC = np_simple_proc.cpp
NP_SIMPLE_PROC_OBJ = np_simple_proc

# 找到 src/ 內的所有 .cpp 檔案
SRC_FILES = $(wildcard src/*.cpp)
BIN_FILES = $(patsubst src/%.cpp,bin/%,$(SRC_FILES))

# 內建指令
BUILD_IN_CMD = ls cat wc
BIN_CMD_FILES = $(patsubst %,bin/%,$(BUILD_IN_CMD))

.PHONY: all clean

all: bin $(NP_SIMPLE_OBJ) $(NP_SIMPLE_PROC_OBJ) $(BIN_FILES) $(BIN_CMD_FILES)

# 建立 bin/ 目錄
bin:
	mkdir -p bin

# 編譯 npsimple.cpp 和 np_simple_proc.cpp（放在 working directory）
$(NP_SIMPLE_OBJ): $(NP_SIMPLE_SRC)
	$(CC) $(CXXFLAGS) $< -g -o $@
$(NP_SIMPLE_PROC_OBJ): $(NP_SIMPLE_PROC_SRC)
	$(CC) $(CXXFLAGS) $< -g -o $@

# 編譯 src/*.cpp 到 bin/
bin/%: src/%.cpp | bin
	$(CC) $(CXXFLAGS) $< -o $@

# 複製 /bin/ 內建指令到 bin/
bin/%: bin
	cp -f $(shell which $(@F)) $@

clean:
	rm -rf bin $(NP_SIMPLE_OBJ) $(NP_SIMPLE_PROC_OBJ)
