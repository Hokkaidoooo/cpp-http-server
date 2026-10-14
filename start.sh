#!/bin/bash

# 启动 Nginx 服务
nginx -g 'daemon off;' &

# 启动您的 C++ 服务器
/usr/src/myapp/myserver
