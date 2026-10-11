// 设备唯一标识的单元测试（文档 2.4「标识不变」）。
//
// deviceid 一旦变化，手机端会把它当成新设备、要求重新配对，
// 所以格式校验和持久化路径必须可靠。
#include <adisplay/common/Config.h>
#include <adisplay/common/DeviceIdentity.h>

#include <set>

#include "AdTest.h"

using namespace adisplay::common;

AD_TEST(generated_device_id_format, "生成的设备标识格式合法") {
    const std::string device_id = generate_device_id();
    AD_CHECK_EQ(device_id.size(), static_cast<std::size_t>(17));
    AD_CHECK(is_valid_device_id(device_id));
}

AD_TEST(device_id_is_locally_administered, "设备标识是本地管理地址") {
    // 第一个字节应当清掉多播位、置上本地管理位（0x02），
    // 避免和局域网里真实网卡的 MAC 冲突。
    for (int i = 0; i < 32; ++i) {
        const std::string device_id = generate_device_id();
        AD_CHECK(is_valid_device_id(device_id));

        const char high = device_id[0];
        const char low = device_id[1];
        const int first_byte = (std::stoi(std::string(1, high), nullptr, 16) << 4) |
                               std::stoi(std::string(1, low), nullptr, 16);

        AD_CHECK_EQ(first_byte & 0x01, 0);   // 多播位必须为 0
        AD_CHECK_EQ(first_byte & 0x02, 2);   // 本地管理位必须为 1
    }
}

AD_TEST(generated_ids_are_unique, "连续生成的标识不重复") {
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        seen.insert(generate_device_id());
    }
    // 6 字节随机，64 次抽样几乎不可能碰撞。
    AD_CHECK_EQ(seen.size(), static_cast<std::size_t>(64));
}

AD_TEST(device_id_validation_rejects, "设备标识校验拒绝非法输入") {
    AD_CHECK(!is_valid_device_id(""));
    AD_CHECK(!is_valid_device_id("AA:BB:CC:DD:EE"));
    AD_CHECK(!is_valid_device_id("AA:BB:CC:DD:EE:FF:00"));
    AD_CHECK(!is_valid_device_id("AA-BB-CC-DD-EE-FF"));   // 分隔符不对
    AD_CHECK(!is_valid_device_id("GG:BB:CC:DD:EE:FF"));   // 非十六进制
    AD_CHECK(!is_valid_device_id("AABBCCDDEEFF"));        // 缺冒号

    AD_CHECK(is_valid_device_id("AA:BB:CC:DD:EE:FF"));
    AD_CHECK(is_valid_device_id("aa:bb:cc:dd:ee:ff"));    // 小写也接受
}

AD_TEST(generated_uuid_format, "生成的UUID格式合法") {
    const std::string uuid = generate_uuid_v4();
    AD_CHECK_EQ(uuid.size(), static_cast<std::size_t>(36));
    AD_CHECK(is_valid_uuid(uuid));
}

AD_TEST(uuid_is_version_4, "UUID是版本4且变体正确") {
    for (int i = 0; i < 32; ++i) {
        const std::string uuid = generate_uuid_v4();
        AD_CHECK(is_valid_uuid(uuid));

        // 第 15 个字符是版本号（"xxxxxxxx-xxxx-Mxxx-..."）
        AD_CHECK_EQ(uuid[14], '4');
        // 第 20 个字符是变体（"....-Nxxx-..."），v4 应为 8/9/a/b
        const char variant = uuid[19];
        AD_CHECK(variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
    }
}

AD_TEST(uuid_validation_rejects, "UUID校验拒绝非法输入") {
    AD_CHECK(!is_valid_uuid(""));
    AD_CHECK(!is_valid_uuid("not-a-uuid"));
    AD_CHECK(!is_valid_uuid("550e8400-e29b-41d4-a716-44665544000"));    // 少一位
    AD_CHECK(!is_valid_uuid("550e8400-e29b-41d4-a716-4466554400000"));  // 多一位
    AD_CHECK(!is_valid_uuid("550e8400xe29b-41d4-a716-446655440000"));   // 分隔符错位

    AD_CHECK(is_valid_uuid("550e8400-e29b-41d4-a716-446655440000"));
}

AD_TEST(identity_from_bytes, "从字节构造标识") {
    const uint8_t six[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t sixteen[16] = {0x55, 0x0e, 0x84, 0x00, 0xe2, 0x9b, 0x41, 0xd4,
                                 0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00};

    const DeviceIdentity identity = DeviceIdentity::from_bytes(six, sixteen);

    AD_CHECK_EQ(identity.device_id(), std::string("02:11:22:33:44:55"));
    AD_CHECK_EQ(identity.uuid(), std::string("550e8400-e29b-41d4-a716-446655440000"));
    AD_CHECK(is_valid_device_id(identity.device_id()));
    AD_CHECK(is_valid_uuid(identity.uuid()));
}

AD_TEST(identity_survives_reload, "标识持久化后保持不变") {
    // 文档 2.4 的核心要求：改名、重启、升级都不能让 deviceid 变化。
    Config config;
    const DeviceIdentity original = DeviceIdentity::from_bytes(
        reinterpret_cast<const uint8_t*>("\x02\xAA\xBB\xCC\xDD\xEE"),
        reinterpret_cast<const uint8_t*>("\x55\x0e\x84\x00\xe2\x9b\x41\xd4"
                                         "\xa7\x16\x44\x66\x55\x44\x00\x00"));
    original.store(config);

    const DeviceIdentity reloaded = DeviceIdentity::load_or_create(config);
    AD_CHECK_EQ(reloaded.device_id(), original.device_id());
    AD_CHECK_EQ(reloaded.uuid(), original.uuid());
}

AD_TEST(identity_created_when_missing, "配置缺失时生成新的标识") {
    const Config empty;
    const DeviceIdentity identity = DeviceIdentity::load_or_create(empty);

    AD_CHECK(is_valid_device_id(identity.device_id()));
    AD_CHECK(is_valid_uuid(identity.uuid()));
}

AD_TEST(identity_regenerated_when_corrupt, "配置中标识被改坏时重新生成") {
    Config config;
    config.set_string("identity.device_id", "这不是一个合法的标识");
    config.set_string("identity.uuid", "也不是");

    const DeviceIdentity identity = DeviceIdentity::load_or_create(config);

    AD_CHECK(is_valid_device_id(identity.device_id()));
    AD_CHECK(is_valid_uuid(identity.uuid()));
    AD_CHECK_NE(identity.device_id(), std::string("这不是一个合法的标识"));
}

int main() {
    return adtest::run_all("设备标识测试");
}
