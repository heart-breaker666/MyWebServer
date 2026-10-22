#!/bin/bash
# 查询 yourdb 数据库中 user 表的全部数据。
# 用法：./query_user.sh

mysql -uroot -ppassword yourdb -e "SELECT * FROM user;"
