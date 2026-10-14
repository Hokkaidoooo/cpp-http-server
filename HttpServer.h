// HttpServer.h
#pragma once
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include "Logger.h"
#include "ThreadPool.h"
#include "Router.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Database.h" 
#include <fstream>
#include <sstream>

// Connection 结构体现在需要包含请求数据的缓冲区和状态信息
struct Connection {
    std::string requestBuffer; // 用于存储从客户端接收到的请求数据
    std::string responseData; // 存储待发送的响应数据
    size_t sentBytes = 0; // 记录已发送的字节数
    bool requestComplete = false; // 标记请求是否已完全接收
    bool responseReady = false; // 标记响应是否准备好发送
};

class HttpServer {
public:
    HttpServer(int port, int max_events, Database& db) : server_fd(-1), epollfd(-1), port(port), max_events(max_events), db(db) {}

 // 启动服务器方法，设置套接字、epoll、启动线程池并进入循环等待处理客户端连接
    void start() {
        setupServerSocket(); // 创建并配置服务器套接字
        setupEpoll(); // 创建并配置epoll实例
        ThreadPool pool(16); // 创建一个拥有16个工作线程的线程池以应对高并发场景
        
        // 初始化epoll_event数组，用于存放epoll_wait返回的就绪事件
        struct epoll_event events[max_events];

        // 主循环，不断等待新的连接请求或已连接套接字上的读写事件
        while (true) {
            int nfds = epoll_wait(epollfd, events, max_events, -1); // 等待epoll事件发生
            
            // 遍历所有就绪事件
            for (int n = 0; n < nfds; ++n) {
                if (events[n].data.fd == server_fd) { // 如果是服务器套接字就绪
                    acceptConnection(); // 接受新连接
                } 
                else if (events[n].events & EPOLLOUT) {
                // 准备发送数据
                pool.enqueue([fd = events[n].data.fd, this]() {
                        this-> sendData(fd); 
                    });  
                }
                else { // 如果是已连接客户端套接字就绪
                    // 使用线程池异步处理该连接上的请求
                    pool.enqueue([fd = events[n].data.fd, this]() {
                        this->handleConnection(fd);
                    });
                }
            }
        }
    }

    // 12新增
    std::string readFile(const std::string& filePath) {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            return "Error: Unable to open file " + filePath;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // 然后在 HttpServer 类中添加一个方法来处理读取操作
void readData(int fd) {
    std::lock_guard<std::mutex> lock(connectionsMutex);
    auto& conn = connections[fd];

    char buffer[4096]; // 临时缓冲区，用于一次性读取数据
    ssize_t bytesRead;

    while ((bytesRead = read(fd, buffer, sizeof(buffer))) > 0) {
        conn.requestBuffer.append(buffer, bytesRead); // 将读取的数据追加到请求缓冲区
    }

    if (bytesRead == -1 && errno != EAGAIN) {
        LOG_ERROR("Error reading from socket %d: %s", fd, strerror(errno));
        close(fd);
        connections.erase(fd);
        return;
    }

    // 检查请求是否包含HTTP请求头和请求体的结束标记 "\r\n\r\n"
    if (!conn.requestComplete && conn.requestBuffer.find("\r\n\r\n") != std::string::npos) {
        conn.requestComplete = true; // 如果找到，标记请求为完整

        // 在这里处理请求，如解析HTTP请求、生成响应等
        // 假设我们现在直接准备响应数据
        HttpRequest request;
        if (request.parse(conn.requestBuffer)) { // 如果请求成功解析
            HttpResponse response = router.routeRequest(request);
            conn.responseData = response.toString(); // 将响应转换为字符串
            conn.responseReady = true; // 标记响应准备好发送
            registerWrite(fd); // 注册EPOLLOUT事件准备发送数据
        }
    }

    // 如果数据还没读取完全，确保EPOLLIN事件继续被注册
    if (!conn.requestComplete) {
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET; // 继续监听读事件
        event.data.fd = fd;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
    }
}


    void setupRoutes() {
        router.addRoute("GET", "/", [](const HttpRequest& req) {
            HttpResponse response;
            response.setStatusCode(200);
            response.setBody("Hello, World!");
            return response;
        });
        router.addRoute("GET", "/login", [this](const HttpRequest& req) {
            HttpResponse response;
            response.setStatusCode(200);
            response.setHeader("Content-Type", "text/html");
            response.setBody(readFile("UI/login.html"));
            return response;
        });

        router.addRoute("GET", "/register", [this](const HttpRequest& req) {
            HttpResponse response;
            response.setStatusCode(200);
            response.setHeader("Content-Type", "text/html");
            response.setBody(readFile("UI/register.html"));
            return response;
        });

        router.addRoute("GET", "/upload", [this](const HttpRequest& req) {
            HttpResponse response;
            response.setStatusCode(200);
            response.setHeader("Content-Type", "text/html");
            response.setBody(readFile("UI/upload.html")); // 确保upload.html位于UI文件夹中
            return response;
        });

        router.setupDatabaseRoutes(db);
        router.setupImageRoutes(db); 
        // ... 添加更多路由 ...

    }

private:
    int server_fd, epollfd, port, max_events;
    Router router;
    Database& db;
    std::unordered_map<int, Connection> connections; // 使用文件描述符作为键
    std::mutex connectionsMutex; // 保护connections的互斥锁

    void setupServerSocket() {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        bind(server_fd, (struct sockaddr *)&address, sizeof(address));
        listen(server_fd, SOMAXCONN);

        setNonBlocking(server_fd);
    }

    void setupEpoll() {
        epollfd = epoll_create1(0);
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = server_fd;
        epoll_ctl(epollfd, EPOLL_CTL_ADD, server_fd, &event);
    }

    void registerWrite(int fd) {
        struct epoll_event event;
        event.events = EPOLLOUT | EPOLLET; // 监听可写事件，使用边缘触发
        event.data.fd = fd;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event); // 修改fd的监听事件为EPOLLOUT
    }


    void acceptConnection() {
        struct sockaddr_in client_addr;
        socklen_t client_addrlen = sizeof(client_addr);
        int client_sock;
        while ((client_sock = accept(server_fd, (struct sockaddr *)&client_addr, &client_addrlen)) > 0) {
            setNonBlocking(client_sock);
            struct epoll_event event = {};
            event.events = EPOLLIN | EPOLLET;
            event.data.fd = client_sock;
            epoll_ctl(epollfd, EPOLL_CTL_ADD, client_sock, &event);
        }
        if (client_sock == -1 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
            LOG_ERROR("Error accepting new connection");
        }
    }
    void sendBadRequestResponse(int fd) {
        const char* response = 
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 47\r\n"
            "\r\n"
            "<html><body><h1>400 Bad Request</h1></body></html>";
        send(fd, response, strlen(response), 0);
    }

    // sendData函数用于将服务器生成的响应数据发送给指定文件描述符（fd）所关联的客户端。
    void sendData(int fd) {
        // 使用std::lock_guard确保在并发环境下对connections容器的安全访问
        std::lock_guard<std::mutex> lock(connectionsMutex);

        // 从connections映射表中获取与当前fd对应的连接信息
        auto& conn = connections[fd];

        // 当已发送的数据量小于总响应数据大小时，继续循环发送剩余数据
        while (conn.sentBytes < conn.responseData.size()) {
            // 调用send系统调用发送剩余数据
            ssize_t sent = send(fd, conn.responseData.c_str() + conn.sentBytes,
                                conn.responseData.size() - conn.sentBytes, 0);

            // 发送成功
            if (sent > 0) {
                // 更新已发送的数据量
                conn.sentBytes += sent;
            }
            // 发送失败但错误为EAGAIN或EWOULDBLOCK，表示套接字暂时不可写，需要等待下一次变为可写时再尝试发送
            else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return; // 返回并等待EPOLLOUT事件触发后再继续发送
            }
            // 其他错误情况，如网络故障等
            else {
                // 记录错误日志
                LOG_ERROR("Error sending data to socket %d", fd);
                
                // 关闭连接并从连接列表中移除该连接信息
                close(fd);
                connections.erase(fd);

                // 结束函数执行
                return;
            }
        }

        // 数据已全部发送完毕，关闭连接并清理连接状态
        close(fd);
        connections.erase(fd);
    }


    
void handleConnection(int fd) {
    // 首先检查这个fd是否已经在connections中有记录
    auto connIt = connections.find(fd);
    if (connIt == connections.end()) {
        // 如果是新连接，初始化连接状态并加入到connections映射中
        Connection newConn;
        std::lock_guard<std::mutex> lock(connectionsMutex);
        connIt = connections.insert({fd, std::move(newConn)}).first;
    }

    auto& conn = connIt->second;

    // 检查是否已经读取到完整的请求
    if (!conn.requestComplete) {
        char buffer[4096]; // 临时缓冲区，用于一次性读取数据
        ssize_t bytes_read;

        // 尝试读取数据
        bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            // 将读取的数据追加到请求缓冲区
            conn.requestBuffer.append(buffer, bytes_read);

            // 检查是否读取到HTTP请求的结束标记"\r\n\r\n"
            if (conn.requestBuffer.find("\r\n\r\n") != std::string::npos) {
                conn.requestComplete = true; // 标记请求为完整
            } else {
                // 如果请求还不完整，继续监听EPOLLIN事件
                return;
            }
        } else if (bytes_read == -1 && errno != EAGAIN) {
            // 读取出错
            LOG_ERROR("Error reading from socket %d: %s", fd, strerror(errno));
            close(fd);
            connections.erase(fd);
            return;
        } else if (bytes_read == 0) {
            // 客户端关闭了连接
            close(fd);
            connections.erase(fd);
            return;
        }
    }

    // 处理完整的请求
    if (conn.requestComplete && !conn.responseReady) {
        HttpRequest request;
        if (request.parse(conn.requestBuffer)) {
            // 如果请求成功解析，生成响应
            HttpResponse response = router.routeRequest(request);
            conn.responseData = response.toString();
            conn.responseReady = true;
            conn.sentBytes = 0;
            // 注册EPOLLOUT事件，准备发送响应数据
            registerWrite(fd);
        } else {
            // 请求解析失败
            LOG_WARNING("Failed to parse request for socket %d", fd);
            sendBadRequestResponse(fd); // 发送400 Bad Request响应
            close(fd);
            connections.erase(fd);
        }
    }
}




    void setNonBlocking(int sock) {
        int flags = fcntl(sock, F_GETFL, 0);
        flags |= O_NONBLOCK;
        fcntl(sock, F_SETFL, flags);
    }
};
