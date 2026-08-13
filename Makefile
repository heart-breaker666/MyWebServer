# MyWebServer 构建文件
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Iinclude
LDFLAGS = -pthread

# 日志系统单元测试
LOG_TEST = log_test
LOG_TEST_SRCS = src/Log.cpp test/LogTest.cpp

.PHONY: all test clean

all: $(LOG_TEST)

$(LOG_TEST): $(LOG_TEST_SRCS) include/Log.h
	$(CXX) $(CXXFLAGS) -o $@ $(LOG_TEST_SRCS) $(LDFLAGS)

test: $(LOG_TEST)
	./$(LOG_TEST)

clean:
	rm -f $(LOG_TEST) *.log *.log.*
