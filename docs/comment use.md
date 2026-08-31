./server -c 1 -m 3 关闭日志启动
wrk -t8 -c1000 -d5s http://127.0.0.1:9006/ 