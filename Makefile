# MyWebServer 构建文件
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Iinclude
LDFLAGS = -pthread

# 主程序：Web 服务器（串联 Config/Log/ConnectionPool/WebServer/HttpConn/ThreadPool/Timer）
SERVER = server
SERVER_SRCS = src/main.cpp src/Config.cpp src/Log.cpp src/ConnectionPool.cpp src/HttpConn.cpp src/WebServer.cpp src/TimerManager.cpp

$(SERVER): $(SERVER_SRCS) include/WebServer.h include/Config.h include/Log.h include/ConnectionPool.h include/HttpConn.h include/ThreadPool.h include/TimerManager.h
	$(CXX) $(CXXFLAGS) -o $@ $(SERVER_SRCS) $(LDFLAGS) -lmysqlclient

clean:
	rm -f $(SERVER) *.log *.log.* *.conf
