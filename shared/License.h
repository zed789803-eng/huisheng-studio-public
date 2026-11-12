#pragma once

#include <cstdint>
#include <string>

// 绘声Studio 离线注册码。
//
// 注册码 = "HS1-" + Crockford-Base32( payload | HMAC-SHA256(密钥, payload)[:6] )
// payload = 0x01 | 名字长度 | 名字(UTF-8)
//
// 不绑机器、不联网、不过期：一个名字一个码，抄错一个字符就验不过。
// 生成端是 tools/hskeygen.cpp（也在 CI 产物里）与开发机上的 keygen.py，
// 两边用同一把密钥、同一套编码，测试向量见 tests/license_test.cpp。
namespace hs::license {

// 0 = 只提示：没有注册码时界面显示"未注册"，音频功能照常工作。
// 1 = 功能受限（当前口径）：没有注册码时 Agent 不启动采集、也不回传，
//     插件出声不了，界面显示"未注册"。
//
// 这是"稍微防一下"，不是防破解：能改二进制的人总能绕过去，它挡的是
// "拿到包不激活就直接用"。
inline constexpr int kEnforcement = 1;

struct Result
{
    bool valid = false;
    std::string serial;   // 码里带的那个名字（UTF-8）
};

// 生成 / 校验（key 可换，测试用；产品用 productKey()）
std::string makeCode(const uint8_t key[32], const std::string& serial);
Result verifyCode(const std::string& input, const uint8_t key[32]);

const uint8_t* productKey();

// 构建时是否注入了真实密钥（shared/LicenseKey.generated.h）。
// 没有注入时 productKey() 返回 32 个 0，只有"本地也能编译"的意义：
// 注册/校验一律失败，测试里的黄金码用例会自己跳过。
bool productKeyInjected();
std::string make(const std::string& serial);
Result verify(const std::string& input);

// ---- 本机激活状态 --------------------------------------------------------
// 存在 ~/Library/Application Support/huishengstudio/license.txt
std::string storeDir();
std::string storePath();
std::string storedCode();
bool storeCode(const std::string& code);

// 校验已保存的码；没有码或码无效时 valid = false
Result current();

// 校验并保存；失败不改动已有状态
Result activate(const std::string& input);

} // namespace hs::license
