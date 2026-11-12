#include "License.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace hs;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    std::printf("%-58s %s\n", name, condition ? "ok" : "FAIL");

    if (! condition)
        ++failures;
}

// 张 三 in UTF-8; escaped so the file does not depend on a source encoding.
const char* kChinese = "\xE5\xBC\xA0\xE4\xB8\x89";

struct Vector
{
    const char* serial;
    const char* code;
};

// Cross-language vectors: keygen.py must print exactly these.
const Vector kVectors[] = {
    { "1",         "HS1-040K-3GJ8-9PXP-TX8" },
    { "abc",       "HS1-041P-2RK3-RVNK-G818-70" },
    { "serial-01", "HS1-044Q-6SBJ-D5GP-RB9G-66RT-C99E-25J0" },
    { kChinese,    "HS1-043E-BF50-WJW8-JJRB-JM4Q-7Z0" },
};

} // namespace

int main()
{
    for (const Vector& vector : kVectors)
    {
        const std::string made = license::make(vector.serial);

        if (made != vector.code)
            std::printf("   made=%s expected=%s\n", made.c_str(), vector.code);

        check(made == vector.code, "make() matches the reference code");

        const license::Result result = license::verify(vector.code);

        check(result.valid, "verify() accepts a freshly made code");
        check(result.serial == vector.serial, "verify() returns the serial");
    }

    // Typing habits: lower case, no prefix, dashes dropped.
    {
        const std::string code = kVectors[2].code;
        std::string relaxed;

        for (char c : code)
        {
            if (c == '-')
                continue;

            relaxed.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        }

        const license::Result result = license::verify(relaxed);
        check(result.valid && result.serial == "serial-01", "lower case and no dashes still activates");
    }

    // Crockford folds the confusables: O -> 0, I/L -> 1.
    {
        std::string code = kVectors[1].code;

        // Leave the "HS1-" prefix alone; only the body is re-typed.
        for (size_t i = 4; i < code.size(); ++i)
        {
            if (code[i] == '0') code[i] = 'O';
            else if (code[i] == '1') code[i] = 'I';
        }

        check(license::verify(code).valid, "O/0 and I/1 are interchangeable");
    }

    // Surrounding whitespace is what a paste gives you.
    check(license::verify("  " + std::string(kVectors[3].code) + "\n").valid, "a pasted code with whitespace activates");

    // Tampering: flip one character of the body.
    {
        std::string code = kVectors[3].code;
        code[code.size() - 2] = (code[code.size() - 2] == 'A' ? 'B' : 'A');
        check(! license::verify(code).valid, "a single wrong character is rejected");
    }

    // Another product's key must not validate our codes.
    {
        uint8_t other[32] = {};

        for (int i = 0; i < 32; ++i)
            other[i] = uint8_t(i * 7 + 1);

        check(! license::verifyCode(kVectors[0].code, other).valid, "a foreign key does not validate");
        check(license::makeCode(other, "abc") != kVectors[1].code, "a foreign key makes different codes");
    }

    // Garbage in.
    check(! license::verify("").valid, "empty input is rejected");
    check(! license::verify("HS1-").valid, "bare prefix is rejected");
    check(! license::verify("hello world").valid, "prose is rejected");
    check(! license::verify(std::string("HS1-") + std::string(60, 'Z')).valid, "over-long input is rejected");

    // Serial length limits: 24 bytes is the maximum.
    check(license::make(std::string(25, 'a')).empty(), "a 25 byte serial is refused");
    check(! license::make(std::string(24, 'a')).empty(), "a 24 byte serial is accepted");
    check(license::make("").empty(), "an empty serial is refused");

    // Round trip through the store helpers without touching the real file.
    {
        const std::string path = license::storePath();
        check(! path.empty(), "store path resolves under HOME");
        check(path.find("huishengstudio") != std::string::npos, "store path is product scoped");
    }

    // The path a buyer actually walks: paste the code, it lands on disk, and the
    // next launch reads it back as licensed. Runs against a throwaway HOME so the
    // machine that builds this is never left activated.
    {
        const std::string tmp = "/tmp/hs-license-test-" + std::to_string(::getpid());
        ::mkdir(tmp.c_str(), 0700);
        ::setenv("HOME", tmp.c_str(), 1);

        const std::string code = license::make(kChinese);

        check(license::storeCode(code), "storeCode() writes the code");
        check(license::current().valid, "current() sees the stored code");
        check(license::current().serial == kChinese, "current() returns the stored serial");

        {
            std::ofstream damaged(license::storePath(), std::ios::trunc);
            damaged << "HS1-0000-0000-0000-0000\n";
        }

        check(! license::current().valid, "a bogus stored code is refused");

        std::remove(license::storePath().c_str());
        check(! license::current().valid, "a fresh install reads as unlicensed");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "license: PASS" : "license: FAIL",
                failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
