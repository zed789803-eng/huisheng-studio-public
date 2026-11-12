// 绘声Studio 注册码生成器（命令行）。
//
// 这是给你（作者）自己用的工具，不要跟着安装包发给别人：它带着产品密钥。
//
//   hskeygen <名字>              生成一个注册码
//   hskeygen --batch <前缀> <n>  批量生成 n 个
//   hskeygen --check <注册码>     校验一个码
//   hskeygen --selftest          自检
#include "License.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int usage()
{
    std::printf(
        "绘声Studio 注册码生成器\n"
        "\n"
        "  hskeygen <名字>              生成一个注册码\n"
        "  hskeygen --batch <前缀> <n>  批量生成 n 个\n"
        "  hskeygen --check <注册码>     校验一个注册码\n"
        "  hskeygen --selftest          自检\n");
    return 1;
}

int selfTest()
{
    const std::string code = hs::license::make("abc");

    if (code != "HS1-041P-2RK3-RVNK-G818-70")
    {
        std::printf("FAIL: make(\"abc\") = %s\n", code.c_str());
        return 1;
    }

    const auto result = hs::license::verify(code);

    if (! result.valid || result.serial != "abc")
    {
        std::printf("FAIL: verify() rejected its own code\n");
        return 1;
    }

    std::printf("hskeygen: PASS (%s)\n", code.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();

    const std::string first = argv[1];

    if (first == "--selftest")
        return selfTest();

    if (first == "--check")
    {
        if (argc < 3)
            return usage();

        const auto result = hs::license::verify(argv[2]);

        if (result.valid)
        {
            std::printf("有效：%s\n", result.serial.c_str());
            return 0;
        }

        std::printf("无效：这个码不是本产品生成的，或者抄错了\n");
        return 1;
    }

    if (first == "--batch")
    {
        if (argc < 4)
            return usage();

        const std::string prefix = argv[2];
        const int count = std::atoi(argv[3]);

        for (int i = 1; i <= count; ++i)
        {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "%02d", i);
            std::printf("%s\n", hs::license::make(prefix + suffix).c_str());
        }

        return 0;
    }

    const std::string code = hs::license::make(first);

    if (code.empty())
    {
        std::printf("名字必须是 1-24 字节（UTF-8）\n");
        return 1;
    }

    std::printf("%s\n", code.c_str());
    return 0;
}
