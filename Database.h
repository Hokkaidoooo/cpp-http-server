#pragma once
#include "Auth.h"
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

class Database final : public UserRepository {
public:
    Database(const std::string& uri, std::string database) : pool_(mongocxx::uri{uri}), database_(std::move(database)) {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto client = pool_.acquire();
        auto db = (*client)[database_];
        db.run_command(make_document(kvp("ping", 1)));
        mongocxx::options::index options;
        options.unique(true);
        db["users"].create_index(make_document(kvp("username", 1)), options);
        const auto legacy = db["users"].find_one(make_document(kvp("password", make_document(kvp("$exists", true)))));
        if (legacy) throw std::runtime_error("检测到旧明文密码记录：请使用新数据库或先完成显式迁移");
    }
    bool insertUser(const std::string& username, const std::string& passwordHash) override {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto client = pool_.acquire();
        try {
            const auto result = (*client)[database_]["users"].insert_one(
                make_document(kvp("username", username), kvp("password_hash", passwordHash)));
            if (!result) throw std::runtime_error("MongoDB 未确认用户写入");
            return true;
        } catch (const mongocxx::operation_exception& error) {
            if (error.code().value() == 11000) return false;
            throw;
        }
    }
    std::optional<std::string> passwordHash(const std::string& username) override {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        auto client = pool_.acquire();
        const auto record = (*client)[database_]["users"].find_one(make_document(kvp("username", username)));
        if (!record) return std::nullopt;
        const auto value = record->view()["password_hash"];
        if (!value || value.type() != bsoncxx::type::k_utf8) throw std::runtime_error("用户密码哈希缺失或类型错误");
        const auto text = value.get_string().value;
        return std::string(text.data(), text.size());
    }
private:
    mongocxx::pool pool_;
    const std::string database_;
};
