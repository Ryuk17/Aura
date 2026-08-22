// 极简 JSON 解析器（自研，零依赖）— 满足配置文件加载需求
// 支持：null / bool / number(double) / string / array / object
// 不支持：unicode 转义（\uXXXX 视为普通字符原样保留）、超大数值精度
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aura::json {

class Value {
public:
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

    Value() : type_(Type::kNull), num_(0), bool_(false) {}
    explicit Value(Type t) : type_(t), num_(0), bool_(false) {}
    explicit Value(bool b) : type_(Type::kBool), num_(0), bool_(b) {}
    explicit Value(double d) : type_(Type::kNumber), num_(d), bool_(false) {}
    explicit Value(std::string s) : type_(Type::kString), num_(0), bool_(false), str_(std::move(s)) {}

    // 解析失败时返回 null 值（is_null() 为 true），可传入错误信息输出
    static Value Parse(const std::string& text, std::string* error = nullptr);

    // 序列化（含缩进）
    std::string Dump() const;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::kNull; }
    bool is_bool() const { return type_ == Type::kBool; }
    bool is_number() const { return type_ == Type::kNumber; }
    bool is_string() const { return type_ == Type::kString; }
    bool is_array() const { return type_ == Type::kArray; }
    bool is_object() const { return type_ == Type::kObject; }

    bool as_bool() const { return bool_; }
    double as_number() const { return num_; }
    int as_int() const { return static_cast<int>(num_); }
    const std::string& as_string() const { return str_; }

    // 数组访问（越界返回 null 值）
    const Value& operator[](size_t index) const;
    size_t size() const { return type_ == Type::kArray ? arr_.size() : 0; }

    // 对象访问（key 不存在返回 null 值）
    const Value& operator[](const std::string& key) const;
    bool Has(const std::string& key) const;
    // 便捷取值：GetInt("key", 默认值) 等
    int GetInt(const std::string& key, int def) const;
    double GetNumber(const std::string& key, double def) const;
    bool GetBool(const std::string& key, bool def) const;
    std::string GetString(const std::string& key, const std::string& def) const;

    // 构建
    void Push(Value v);
    void Set(const std::string& key, Value v);

private:
    // 递归下降解析（内部状态）
    struct Parser {
        const std::string& s;
        size_t pos = 0;
        std::string error;
        bool ok = true;

        Value ParseValue();
        void SkipWs();
        bool Consume(char c);
        Value ParseString();
        Value ParseNumber();
        Value ParseArray();
        Value ParseObject();
    };

    Type type_;
    double num_;
    bool bool_;
    std::string str_;
    std::vector<Value> arr_;
    std::map<std::string, Value> obj_;
};

// 顶层类型包装（null 值共享）
const Value& Null();

}  // namespace aura::json
