// ADisplay —— 极简单元测试框架
//
// 刻意不引入 gtest：核心库的测试只需要断言和用例注册，多一个第三方依赖
// 就多一处 CI 失败点。文档第 10 节要求的「解析逻辑单元测试」用这个框架
// 足够表达。
#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace adtest {

struct AssertionFailure {
    std::string message;
};

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> instance;
    return instance;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back(TestCase{std::string(name), std::move(body)});
    }
};

// 断言失败时抛这个异常，由 runner 捕获并记为失败。
inline void fail(const char* file, int line, const std::string& message) {
    std::ostringstream out;
    out << file << ":" << line << "  " << message;
    throw AssertionFailure{out.str()};
}

// 把任意值转成可读字符串，用于失败信息。
template <typename T>
std::string describe(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string describe(const std::string& value) {
    return "\"" + value + "\"";
}

// 没有这个重载的话，const char* 会走通用模板并打印出指针地址。
inline std::string describe(const char* value) {
    if (value == nullptr) {
        return "(null)";
    }
    return "\"" + std::string(value) + "\"";
}

inline std::string describe(bool value) {
    return value ? "true" : "false";
}

inline std::string describe(const std::vector<unsigned char>& value) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i != 0) {
            out << " ";
        }
        out << static_cast<int>(value[i]);
    }
    out << "]";
    return out.str();
}

inline int run_all(const char* suite_name) {
    int failed = 0;
    int passed = 0;

    for (const TestCase& test : registry()) {
        try {
            test.body();
            ++passed;
            std::cout << "  [ok]   " << test.name << "\n";
        } catch (const AssertionFailure& failure) {
            ++failed;
            std::cout << "  [FAIL] " << test.name << "\n         " << failure.message << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cout << "  [FAIL] " << test.name << "\n         意外异常：" << ex.what() << "\n";
        } catch (...) {
            ++failed;
            std::cout << "  [FAIL] " << test.name << "\n         意外异常（非 std::exception）\n";
        }
    }

    std::cout << "\n" << suite_name << "：通过 " << passed
              << "，失败 " << failed << "，共 " << registry().size() << "\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace adtest

// test_id 必须是 ASCII 标识符，display_name 是中文描述。
// 刻意不让中文直接当标识符：虽然 C++11 起支持 Unicode 标识符，但
// MSVC 上还要额外依赖 /utf-8，而 CI 一轮要等很久，不值得冒这个险。
#define AD_TEST(test_id, display_name)                                       \
    static void test_id();                                                   \
    static ::adtest::Registrar adtest_registrar_##test_id(display_name, test_id); \
    static void test_id()

#define AD_CHECK(condition)                                                  \
    do {                                                                     \
        if (!(condition)) {                                                  \
            ::adtest::fail(__FILE__, __LINE__, "断言失败：" #condition);     \
        }                                                                    \
    } while (false)

#define AD_CHECK_EQ(actual, expected)                                        \
    do {                                                                     \
        const auto& adtest_actual_value = (actual);                          \
        const auto& adtest_expected_value = (expected);                      \
        if (!(adtest_actual_value == adtest_expected_value)) {               \
            ::adtest::fail(__FILE__, __LINE__,                               \
                           std::string("期望 ") + ::adtest::describe(adtest_expected_value) + \
                           "，实际 " + ::adtest::describe(adtest_actual_value));             \
        }                                                                    \
    } while (false)

#define AD_CHECK_NE(actual, unexpected)                                      \
    do {                                                                     \
        if ((actual) == (unexpected)) {                                      \
            ::adtest::fail(__FILE__, __LINE__, "不应等于 " #unexpected);     \
        }                                                                    \
    } while (false)
