#!/bin/bash

# 更新软件包
apt-get update

# 安装必要的库
apt-get install -y git openssl libssl-dev libsasl2-dev cmake openssl nano

# 下载 MongoDB C++ 驱动，为了方便同学我已经下载好了
# wget -O mongo-cxx-driver-r3.9.0.tar.gz https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.9.0/mongo-cxx-driver-r3.9.0.tar.gz
apt-get install -y libboost-all-dev &&     rm -rf /var/lib/apt/lists/
# 解压并安装 MongoDB C++ 驱动
tar -xzf mongo-cxx-driver-r3.9.0.tar.gz && \
cd mongo-cxx-driver-r3.9.0/build && \
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DBSONCXX_POLY_USE_BOOST=1 \
         -DMONGOCXX_OVERRIDE_DEFAULT_INSTALL_PREFIX=OFF \
         -DBUILD_VERSION=3.9.0
cmake --build .
cmake --build . --target install