/**
 * @file test_tuikey.cpp
 * @brief Unit tests for the key decoder of the SSH text browser (stage 225).
 *
 * A terminal sends keys as byte sequences, and the sequences a browser must know
 * are the ones the clients actually produce: CSI arrows, SS3 function keys, the
 * numeric forms for F5..F12, and the Alt-modified form of F1/F2 that switches a
 * panel's volume. Getting one of them wrong shows up as a key that "does nothing",
 * which is why the decoder is pure and this test is the proof.
 *
 * There is nothing to compare against in the past: the header did not exist before
 * this change, so the test does not compile against `HEAD`.
 *
 * Build (MinGW) — see README.md "Testing":
 *   g++ -std=c++17 -Wall -Wextra -Werror -DDHCP_TEST_HOST \
 *       -Dapp_main=esp_test_app_main -Itest/stubs -I. -Isrc \
 *       test/test_tuikey.cpp host_main.cpp -o test_tuikey
 */

using namespace std;

#ifdef DHCP_TEST_HOST

#include <cstdio>
#include <string>
#include <vector>

#include "../src/net/TuiKey.h"

#define TEST_ASSERT_TRUE(cond)  do { if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define TEST_ASSERT_STREQ(a, b) do { \
        const string _a = (a); const string _b = (b); \
        if (_a != _b) { printf("FAIL: %s:%d: %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, \
                               #a " == " #b, _a.c_str(), _b.c_str()); return 1; } \
    } while(0)

using namespace dhcp::net;

namespace {

string kindName(TuiKeyKind kind)
{
    switch (kind) {
        case TuiKeyKind::None:       return "none";
        case TuiKeyKind::Char:       return "char";
        case TuiKeyKind::Up:         return "up";
        case TuiKeyKind::Down:       return "down";
        case TuiKeyKind::Left:       return "left";
        case TuiKeyKind::Right:      return "right";
        case TuiKeyKind::Home:       return "home";
        case TuiKeyKind::End:        return "end";
        case TuiKeyKind::PageUp:     return "pageup";
        case TuiKeyKind::PageDown:   return "pagedown";
        case TuiKeyKind::Enter:      return "enter";
        case TuiKeyKind::Backspace:  return "backspace";
        case TuiKeyKind::Tab:        return "tab";
        case TuiKeyKind::Escape:     return "escape";
        case TuiKeyKind::F1:         return "f1";
        case TuiKeyKind::F2:         return "f2";
        case TuiKeyKind::F3:         return "f3";
        case TuiKeyKind::F4:         return "f4";
        case TuiKeyKind::F5:         return "f5";
        case TuiKeyKind::F6:         return "f6";
        case TuiKeyKind::F7:         return "f7";
        case TuiKeyKind::F8:         return "f8";
        case TuiKeyKind::F9:         return "f9";
        case TuiKeyKind::F10:        return "f10";
        case TuiKeyKind::F11:        return "f11";
        case TuiKeyKind::F12:        return "f12";
        case TuiKeyKind::AltF1:      return "altf1";
        case TuiKeyKind::AltF2:      return "altf2";
        case TuiKeyKind::CtrlC:      return "ctrlc";
    }
    return "?";
}

/** The names of the keys one chunk decodes into, so a whole sequence is one assert. */
string decode(const string& bytes)
{
    string out;
    for (const TuiKey& key : decodeTuiKeys(bytes)) {
        if (key.kind == TuiKeyKind::Char) {
            out += "Char(";
            out += key.ch;
            out += ") ";
        } else {
            out += kindName(key.kind);
            out += " ";
        }
    }
    if (!out.empty()) out.pop_back();
    return out;
}

/** Plain characters, and the three keys that are a single byte. */
int test_the_single_bytes()
{
    TEST_ASSERT_STREQ(decode("a"), "Char(a)");
    TEST_ASSERT_STREQ(decode("Z"), "Char(Z)");
    TEST_ASSERT_STREQ(decode(" "), "Char( )");
    TEST_ASSERT_STREQ(decode("\r"), "enter");
    TEST_ASSERT_STREQ(decode("\n"), "enter");
    TEST_ASSERT_STREQ(decode("\t"), "tab");
    TEST_ASSERT_STREQ(decode(string(1, '\x7f')), "backspace");
    TEST_ASSERT_STREQ(decode(string(1, '\x08')), "backspace");
    TEST_ASSERT_STREQ(decode(string(1, '\x03')), "ctrlc");
    TEST_ASSERT_STREQ(decode("ab"), "Char(a) Char(b)");
    return 0;
}

/** The arrows, as both CSI and SS3 clients send them. */
int test_the_arrows()
{
    TEST_ASSERT_STREQ(decode("\x1b[A"), "up");
    TEST_ASSERT_STREQ(decode("\x1b[B"), "down");
    TEST_ASSERT_STREQ(decode("\x1b[C"), "right");
    TEST_ASSERT_STREQ(decode("\x1b[D"), "left");
    TEST_ASSERT_STREQ(decode("\x1bOA"), "up");
    TEST_ASSERT_STREQ(decode("\x1bOD"), "left");
    TEST_ASSERT_STREQ(decode("\x1b[H"), "home");
    TEST_ASSERT_STREQ(decode("\x1b[F"), "end");
    TEST_ASSERT_STREQ(decode("\x1b[1~"), "home");
    TEST_ASSERT_STREQ(decode("\x1b[4~"), "end");
    TEST_ASSERT_STREQ(decode("\x1b[5~"), "pageup");
    TEST_ASSERT_STREQ(decode("\x1b[6~"), "pagedown");
    return 0;
}

/** Function keys: the SS3 form for F1..F4, the numeric one for the rest. */
int test_the_function_keys()
{
    TEST_ASSERT_STREQ(decode("\x1bOP"), "f1");
    TEST_ASSERT_STREQ(decode("\x1bOQ"), "f2");
    TEST_ASSERT_STREQ(decode("\x1bOR"), "f3");
    TEST_ASSERT_STREQ(decode("\x1bOS"), "f4");
    TEST_ASSERT_STREQ(decode("\x1b[11~"), "f1");
    TEST_ASSERT_STREQ(decode("\x1b[14~"), "f4");
    TEST_ASSERT_STREQ(decode("\x1b[15~"), "f5");
    TEST_ASSERT_STREQ(decode("\x1b[17~"), "f6");
    TEST_ASSERT_STREQ(decode("\x1b[18~"), "f7");
    TEST_ASSERT_STREQ(decode("\x1b[19~"), "f8");
    TEST_ASSERT_STREQ(decode("\x1b[20~"), "f9");
    TEST_ASSERT_STREQ(decode("\x1b[21~"), "f10");
    TEST_ASSERT_STREQ(decode("\x1b[23~"), "f11");
    TEST_ASSERT_STREQ(decode("\x1b[24~"), "f12");
    return 0;
}

/** Alt+F1 and Alt+F2 are the panel volume switch, in the forms clients send. */
int test_the_alt_function_keys()
{
    TEST_ASSERT_STREQ(decode("\x1b[1;3P"), "altf1");
    TEST_ASSERT_STREQ(decode("\x1b[1;3Q"), "altf2");
    return 0;
}

/** A lone ESC is Escape; an unknown sequence does not swallow what follows it. */
int test_the_odd_sequences()
{
    TEST_ASSERT_STREQ(decode("\x1b"), "escape");
    TEST_ASSERT_STREQ(decode("\x1b[3~"), "");
    TEST_ASSERT_STREQ(decode("\x1b[99~"), "");
    TEST_ASSERT_STREQ(decode("\x1b[1;3Pq"), "altf1 Char(q)");
    TEST_ASSERT_STREQ(decode("ab\x1b[Ac"), "Char(a) Char(b) up Char(c)");
    return 0;
}

} // namespace

extern "C" {

void app_main()
{
    printf("Running TuiKey tests...\n");
    int failures = 0;

    failures += test_the_single_bytes();
    failures += test_the_arrows();
    failures += test_the_function_keys();
    failures += test_the_alt_function_keys();
    failures += test_the_odd_sequences();

    if (failures == 0) {
        printf("All TuiKey tests PASSED!\n");
    } else {
        printf("Some TuiKey tests FAILED (%d)!\n", failures);
    }
}

} // extern "C"

#endif // DHCP_TEST_HOST
