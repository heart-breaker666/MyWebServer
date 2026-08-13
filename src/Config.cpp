#include "../include/Config.h"

#include <string>
#include <unistd.h>

Config::Config()
    : port_(9006),
      log_write_mode_(0),
      listen_trigger_mode_(0),
      conn_trigger_mode_(0),
      sql_pool_size_(8),
      thread_pool_size_(8),
      close_log_(0),
      concurrency_model_(0) {}

void Config::ParseArg(int argc, char* argv[]) {
    int opt;
    const char* optstr = "p:l:m:s:t:c:a:";
    while ((opt = getopt(argc, argv, optstr)) != -1) {
        switch (opt) {
            case 'p':
                port_ = std::stoi(optarg);
                break;
            case 'l':
                log_write_mode_ = std::stoi(optarg);
                break;
            case 'm': {
                // 组合模式：0=LT+LT 1=LT+ET 2=ET+LT 3=ET+ET
                const int mode = std::stoi(optarg);
                listen_trigger_mode_ = (mode == 2 || mode == 3) ? 1 : 0;
                conn_trigger_mode_ = (mode == 1 || mode == 3) ? 1 : 0;
                break;
            }
            case 's':
                sql_pool_size_ = std::stoi(optarg);
                break;
            case 't':
                thread_pool_size_ = std::stoi(optarg);
                break;
            case 'c':
                close_log_ = std::stoi(optarg);
                break;
            case 'a':
                concurrency_model_ = std::stoi(optarg);
                break;
            default:
                break;
        }
    }
}

int Config::GetPort() const {
    return port_;
}

bool Config::IsAsyncLog() const {
    return log_write_mode_ != 0;
}

bool Config::IsListenfdET() const {
    return listen_trigger_mode_ != 0;
}

bool Config::IsConnfdET() const {
    return conn_trigger_mode_ != 0;
}

int Config::GetSqlPoolSize() const {
    return sql_pool_size_;
}

int Config::GetThreadPoolSize() const {
    return thread_pool_size_;
}

bool Config::GetCloseLog() const {
    return close_log_ != 0;
}

bool Config::IsReactorModel() const {
    return concurrency_model_ != 0;
}
