#ifndef DATABASE_H
#define DATABASE_H

#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <string>
#include <future>
#include <iostream> // 添加标准输出库

class Database {
private:
    mongocxx::instance instance{}; // 全局实例
    mongocxx::client client;       // MongoDB 客户端
    mongocxx::database db;         // 数据库

public:
    // 构造函数
    Database(const std::string& uri) {
        LOG_INFO("Connecting to MongoDB");
        std::cout << "Connecting to MongoDB at: " << uri << std::endl;
        client = mongocxx::client{mongocxx::uri{uri}};
        db = client["userdb"]; // 使用 userdb 数据库
    }

    // 异步注册用户
    std::future<bool> registerUserAsync(const std::string& username, const std::string& password) {
        return std::async(std::launch::async, [this, username, password]() {
            return this->registerUser(username, password);
        });
    }

    // 异步登录用户
    std::future<bool> loginUserAsync(const std::string& username, const std::string& password) {
        return std::async(std::launch::async, [this, username, password]() {
            return this->loginUser(username, password);
        });
    }

    // 注册用户
    bool registerUser(const std::string& username, const std::string& password) {
        std::cout << "registerUser begin" << std::endl; // 调试信息
        LOG_INFO("User Register");
        bsoncxx::builder::stream::document document{};
        document << "username" << username << "password" << password;

        auto collection = db["users"];
        bsoncxx::stdx::optional<mongocxx::result::insert_one> result = collection.insert_one(document.view());

        return result ? true : false;
    }

    // 登录用户
    bool loginUser(const std::string& username, const std::string& password) {
        LOG_INFO("User Login");
        auto collection = db["users"];
        bsoncxx::builder::stream::document document{};
        document << "username" << username;

        auto cursor = collection.find(document.view());
        for (auto&& doc : cursor) {
            std::string stored_password = doc["password"].get_utf8().value.to_string();
            if (password == stored_password) {
                return true;
            }
        }
        return false;
    }
// 17新增
     // 存储图片信息
    bool storeImage(const std::string& imageName, const std::string& imagePath, const std::string& description) {
        bsoncxx::builder::stream::document document{};
        document << "name" << imageName
                 << "path" << imagePath
                 << "description" << description;

        auto collection = db["images"];
        bsoncxx::stdx::optional<mongocxx::result::insert_one> result = collection.insert_one(document.view());
        return result ? true : false;
    }

    // 获取图片列表
    std::vector<std::string> getImageList() {
        std::vector<std::string> images;
        auto collection = db["images"];
        auto cursor = collection.find({});
        for (auto&& doc : cursor) {
            images.push_back(doc["path"].get_utf8().value.to_string());
        }
        return images;
    }
};

#endif // DATABASE_H
