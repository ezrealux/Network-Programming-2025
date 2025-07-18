CC = g++
CXXFLAGS = -Wall -O2 -std=c++17

# 設定 npshell
NP_SHELL_SRC = npshell_OOP.cpp
NP_SHELL_BIN = npshell  # 直接在 working directory 下

# 找到 src/ 內的所有 .cpp 檔案
SRC_FILES = $(wildcard src/*.cpp)
BIN_FILES = $(patsubst src/%.cpp,bin/%,$(SRC_FILES))

# 內建指令
BUILD_IN_CMD = ls cat wc
BIN_CMD_FILES = $(patsubst %,bin/%,$(BUILD_IN_CMD))

.PHONY: all clean

all: bin $(NP_SHELL_BIN) $(BIN_FILES) $(BIN_CMD_FILES)

# 建立 bin/ 目錄
bin:
	mkdir -p bin

# 編譯 npshell.cpp（放在 working directory）
$(NP_SHELL_BIN): $(NP_SHELL_SRC)
	$(CC) $(CXXFLAGS) $< -g -o $@

# 編譯 src/*.cpp 到 bin/
bin/%: src/%.cpp | bin
	$(CC) $(CXXFLAGS) $< -o $@

# 複製 /bin/ 內建指令到 bin/
bin/%: bin
	cp -f $(shell which $(@F)) $@

clean:
	rm -rf bin $(NP_SHELL_BIN)
