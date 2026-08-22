// utils/ 模块测试：日志、JSON、WAV
#include "test_framework.h"

#include "json.h"
#include "log.h"
#include "wav.h"

#include <cmath>
#include <string>

namespace aura::test {

TEST_CASE(log_basic) {
    Log::Instance().SetLevel(LogLevel::kTrace);
    ALOG_INFO("test", "hello %d %s", 42, "world");
    auto recent = Log::Instance().Recent(10);
    CHECK(recent.size() >= 1);
    CHECK(recent.back().message == "hello 42 world");
    CHECK(recent.back().level == LogLevel::kInfo);
    CHECK(recent.back().tag == "test");
}

TEST_CASE(log_ring_capacity) {
    for (int i = 0; i < 300; ++i) {
        ALOG_DEBUG("ring", "msg %d", i);
    }
    auto recent = Log::Instance().Recent(1000);
    CHECK(recent.size() <= 256);  // 环形缓冲容量
    CHECK(recent.back().message == "msg 299");
    CHECK(recent[0].message != "msg 0");  // 最旧的已被覆盖
}

TEST_CASE(json_parse_basic) {
    std::string text = R"({
        "name": "aura",
        "version": 0.1,
        "enabled": true,
        "tags": ["audio", "agent"],
        "nested": {"a": 1, "b": "x"},
        "nothing": null
    })";
    auto v = json::Value::Parse(text);
    CHECK(!v.is_null());
    CHECK(v.is_object());
    CHECK(v["name"].as_string() == "aura");
    CHECK(std::fabs(v["version"].as_number() - 0.1) < 1e-9);
    CHECK(v["enabled"].as_bool() == true);
    CHECK(v["tags"].size() == 2);
    CHECK(v["tags"][1].as_string() == "agent");
    CHECK(v["nested"]["a"].as_int() == 1);
    CHECK(v["nothing"].is_null());
    CHECK(v["missing_key"].is_null());
    CHECK(v.GetString("name", "") == "aura");
    CHECK(v.GetInt("nested", -1) == -1);  // 非 int 字段返回默认
}

TEST_CASE(json_parse_errors) {
    std::string err;
    CHECK(json::Value::Parse("", &err).is_null());
    CHECK(!err.empty());
    CHECK(json::Value::Parse("{invalid", &err).is_null());
    CHECK(json::Value::Parse("[1,2", &err).is_null());
    CHECK(json::Value::Parse("{\"a\":1} extra", &err).is_null());
    CHECK(json::Value::Parse("true", &err).as_bool() == true);
    CHECK(json::Value::Parse("-3.5", &err).as_number() == -3.5);
    CHECK(json::Value::Parse("\"str\"", &err).as_string() == "str");
}

TEST_CASE(json_roundtrip) {
    std::string text = R"({"a":[1,2.5,"x"],"b":{"c":false}})";
    auto v = json::Value::Parse(text);
    CHECK(!v.is_null());
    auto dumped = v.Dump();
    auto v2 = json::Value::Parse(dumped);
    CHECK(!v2.is_null());
    CHECK(v2["a"][1].as_number() == 2.5);
    CHECK(v2["b"]["c"].as_bool() == false);
}

TEST_CASE(wav_roundtrip) {
    const std::string path = "test_roundtrip.wav";
    std::vector<int16_t> samples;
    for (int i = 0; i < 16000; ++i) {
        samples.push_back(static_cast<int16_t>(std::sin(2 * 3.14159 * 440 * i / 16000) * 3000));
    }
    CHECK(WriteWav(path, samples, 16000, 1));

    std::vector<int16_t> read;
    WavInfo info;
    CHECK(ReadWav(path, &read, &info));
    CHECK(info.sample_rate == 16000);
    CHECK(info.channels == 1);
    CHECK(info.bits_per_sample == 16);
    CHECK(read.size() == samples.size());
    bool same = true;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (read[i] != samples[i]) {
            same = false;
            break;
        }
    }
    CHECK(same);
    remove(path.c_str());
}

TEST_CASE(wav_read_missing) {
    std::vector<int16_t> samples;
    WavInfo info;
    CHECK(!ReadWav("no_such_file.wav", &samples, &info));
}

}  // namespace aura::test
