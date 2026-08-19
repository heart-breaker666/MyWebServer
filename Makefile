# MyWebServer 构建文件
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Iinclude
LDFLAGS = -pthread

# 日志系统单元测试
LOG_TEST = log_test
LOG_TEST_SRCS = src/Log.cpp test/LogTest.cpp

# 日志系统高并发压力测试（1000 线程并发写）
STRESS_TEST = log_stress_test
STRESS_TEST_SRCS = src/Log.cpp test/LogStressTest.cpp

# 配置模块单元测试
CONFIG_TEST = config_test
CONFIG_TEST_SRCS = src/Config.cpp test/ConfigTest.cpp

# 数据库连接池单元测试（依赖 MySQL 开发库与本地 MySQL 服务）
POOL_TEST = pool_test
POOL_TEST_SRCS = src/ConnectionPool.cpp src/Log.cpp test/ConnectionPoolTest.cpp

# HTTP 连接处理单元测试（socketpair 模拟连接，验证状态机）
HTTP_TEST = http_test
HTTP_TEST_SRCS = src/HttpConn.cpp src/Log.cpp src/ConnectionPool.cpp test/HttpConnTest.cpp

# 线程池单元测试（FIFO/并发/HttpConn 任务/状态管理）
THREADPOOL_TEST = threadpool_test
THREADPOOL_TEST_SRCS = src/HttpConn.cpp src/Log.cpp src/ConnectionPool.cpp test/ThreadPoolTest.cpp

# 主程序：Web 服务器（串联 Config/Log/ConnectionPool/WebServer/HttpConn）
SERVER = server
SERVER_SRCS = src/main.cpp src/Config.cpp src/Log.cpp src/ConnectionPool.cpp src/HttpConn.cpp src/WebServer.cpp

.PHONY: all test stress clean

all: $(LOG_TEST) $(CONFIG_TEST) $(STRESS_TEST) $(POOL_TEST) $(HTTP_TEST) $(THREADPOOL_TEST) $(SERVER)

$(LOG_TEST): $(LOG_TEST_SRCS) include/Log.h
	$(CXX) $(CXXFLAGS) -o $@ $(LOG_TEST_SRCS) $(LDFLAGS)

$(STRESS_TEST): $(STRESS_TEST_SRCS) include/Log.h
	$(CXX) $(CXXFLAGS) -o $@ $(STRESS_TEST_SRCS) $(LDFLAGS)

$(CONFIG_TEST): $(CONFIG_TEST_SRCS) include/Config.h
	$(CXX) $(CXXFLAGS) -o $@ $(CONFIG_TEST_SRCS) $(LDFLAGS)

$(POOL_TEST): $(POOL_TEST_SRCS) include/ConnectionPool.h include/Log.h
	$(CXX) $(CXXFLAGS) -o $@ $(POOL_TEST_SRCS) $(LDFLAGS) -lmysqlclient

$(HTTP_TEST): $(HTTP_TEST_SRCS) include/HttpConn.h include/Log.h include/ConnectionPool.h
	$(CXX) $(CXXFLAGS) -o $@ $(HTTP_TEST_SRCS) $(LDFLAGS) -lmysqlclient

$(THREADPOOL_TEST): $(THREADPOOL_TEST_SRCS) include/ThreadPool.h include/HttpConn.h include/Log.h include/ConnectionPool.h
	$(CXX) $(CXXFLAGS) -o $@ $(THREADPOOL_TEST_SRCS) $(LDFLAGS) -lmysqlclient

$(SERVER): $(SERVER_SRCS) include/WebServer.h include/Config.h include/Log.h include/ConnectionPool.h include/HttpConn.h include/ThreadPool.h
	$(CXX) $(CXXFLAGS) -o $@ $(SERVER_SRCS) $(LDFLAGS) -lmysqlclient

test: $(LOG_TEST) $(CONFIG_TEST) $(POOL_TEST) $(HTTP_TEST) $(THREADPOOL_TEST)
	./$(LOG_TEST)
	./$(CONFIG_TEST)
	./$(POOL_TEST)
	./$(HTTP_TEST)
	./$(THREADPOOL_TEST)

stress: $(STRESS_TEST)
	./$(STRESS_TEST)

clean:
	rm -f $(LOG_TEST) $(CONFIG_TEST) $(STRESS_TEST) $(POOL_TEST) $(HTTP_TEST) $(THREADPOOL_TEST) $(SERVER) *.log *.log.* *.conf

