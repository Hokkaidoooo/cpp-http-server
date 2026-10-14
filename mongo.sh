#!/bin/bash

# 创建 Docker 网络（如果尚未存在）
docker network create mynetwork


# 启动 MongoDB 容器
docker run -d --network mynetwork -p 27017:27017 mongo

# 运行您的应用程序（根据您的需求进行修改）
 docker run -it --network mynetwork -p 9002:80 -p 9003:8080 -p 9004:80801 -p 9005:8082 juanbing/juanbing-server bash
