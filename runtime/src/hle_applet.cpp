// Library applets the game starts. The software keyboard (nn::swkbd::ShowKeyboard) becomes a PC
// text-entry dialog; the sdk's own setters fill the arguments as on the Switch.
//
// ShowKeyboardArg (SDK 5.x, from the sdk's MakePreset/Set*/ShowKeyboard): the 0x400-byte
// KeyboardConfig, then the work buffer (+0x400 pointer, +0x408 size) and the text check work
// buffer (+0x410). KeyboardConfig: +0x24 header text, +0xA6 sub text, +0x1A8 guide text (UTF-16),
// +0x3AC maximum and +0x3B0 minimum length, +0x3BD UTF-8 output, +0x3C0/+0x3C4 the initial text's
// offset in the work buffer and its length, +0x3D0 text check enabled, +0x3D8 its callback
// TextCheckResult(void* check_buffer, uint64_t* message_length, String*): 0 accept, 1 show the
// message in the check buffer and ask again, 2 ask for confirmation.
// String: {char16_t* buffer, uint64_t size in bytes}.
#include <windows.h>

#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "hle.h"

namespace rt {
namespace applet {

constexpr uint32_t kResultCanceled = 0x29F;

std::mutex g_script_lock;
std::deque<std::u16string> g_script;          // --keyboard answers, in order

std::u16string utf16(const char16_t* p, size_t max) {
    std::u16string s;
    for (size_t i = 0; i < max && p[i]; i++) s.push_back(p[i]);
    return s;
}
std::u16string utf8_to_16(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return std::u16string(w.begin(), w.end());
}
std::string utf16_to_8(const std::u16string& s) {
    std::wstring w(s.begin(), s.end());
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
}

// write `text` into the game's String (UTF-16 or UTF-8, null-terminated, clipped to its size)
void store(uint64_t str, const std::u16string& text, bool utf8) {
    uint8_t* buf = (uint8_t*)(uintptr_t)*(uint64_t*)(uintptr_t)str;
    uint64_t size = *(uint64_t*)(uintptr_t)(str + 8);
    if (!buf || size < 2) return;
    if (utf8) {
        std::string u = utf16_to_8(text);
        size_t n = std::min<size_t>(u.size(), (size_t)size - 1);
        memcpy(buf, u.data(), n);
        buf[n] = 0;
    } else {
        size_t n = std::min<size_t>(text.size(), (size_t)size / 2 - 1);
        memcpy(buf, text.data(), n * 2);
        ((char16_t*)buf)[n] = 0;
    }
}

// ShowKeyboard(String* out, ShowKeyboardArg const& arg) -> Result
void ShowKeyboard(Ctx* c) {
    uint64_t out = c->x[0], arg = c->x[1];
    const uint8_t* cfg = (const uint8_t*)(uintptr_t)arg;
    std::u16string header = utf16((const char16_t*)(cfg + 0x24), 0x41), sub = utf16((const char16_t*)(cfg + 0xA6), 0x81),
                   guide = utf16((const char16_t*)(cfg + 0x1A8), 0x101);
    int32_t max_len = *(const int32_t*)(cfg + 0x3AC), min_len = *(const int32_t*)(cfg + 0x3B0);
    bool utf8 = cfg[0x3BD] != 0, check = cfg[0x3D0] != 0;
    uint64_t callback = *(const uint64_t*)(cfg + 0x3D8), work = *(const uint64_t*)(cfg + 0x400),
             check_buf = *(const uint64_t*)(cfg + 0x410);
    int32_t init_off = *(const int32_t*)(cfg + 0x3C0), init_len = *(const int32_t*)(cfg + 0x3C4);
    std::u16string text;
    if (work && init_len > 0 && init_off >= 0) text = utf16((const char16_t*)(uintptr_t)(work + (uint64_t)init_off), (size_t)init_len);
    std::u16string prompt = header;
    if (!sub.empty()) prompt += (prompt.empty() ? u"" : u"\n") + sub;
    if (prompt.empty()) prompt = guide;
    log("swkbd: \"%s\" (up to %d characters)", utf16_to_8(prompt).c_str(), max_len);

    for (int attempt = 0;; attempt++) {
        bool ok;
        {
            std::lock_guard<std::mutex> lk(g_script_lock);
            if (!g_script.empty()) {
                text = g_script.front();
                g_script.pop_front();
                ok = true;
            } else if (!platform_headless()) {
                ok = false;
            } else {
                // headless with no answer left: accept the initial text, or a name for name entry
                if (text.empty()) text = u"Red";
                ok = true;
            }
        }
        if (!ok && !platform_text_input(prompt, guide, text, max_len > 0 ? max_len : 500)) {
            log("swkbd: canceled");
            c->x[0] = kResultCanceled;
            return;
        }
        if (max_len > 0 && (int32_t)text.size() > max_len) text.resize((size_t)max_len);
        if ((int32_t)text.size() < min_len && attempt < 20) {
            platform_message(u"Enter at least " + utf8_to_16(std::to_string(min_len)) + u" characters.", false);
            continue;
        }
        store(out, text, utf8);
        if (check && callback && check_buf) {
            uint64_t msg_len = 0;
            memset((void*)(uintptr_t)check_buf, 0, 0x7D4);
            uint32_t r = (uint32_t)guest_call(c, callback, {check_buf, (uint64_t)(uintptr_t)&msg_len, out});
            std::u16string msg = utf16((const char16_t*)(uintptr_t)check_buf, 0x7D4 / 2);
            if (r == 1) {
                log("swkbd: the game rejected \"%s\": %s", utf16_to_8(text).c_str(), utf16_to_8(msg).c_str());
                if (platform_headless()) {
                    c->x[0] = kResultCanceled;
                    return;
                }
                platform_message(msg, false);
                continue;
            }
            if (r == 2 && !platform_headless() && !platform_message(msg, true)) continue;
        }
        log("swkbd: entered \"%s\"", utf16_to_8(text).c_str());
        c->x[0] = 0;
        return;
    }
}

}  // namespace applet

void keyboard_script(const std::string& spec) {
    std::lock_guard<std::mutex> lk(applet::g_script_lock);
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t end = spec.find(';', pos);
        if (end == std::string::npos) end = spec.size();
        applet::g_script.push_back(applet::utf8_to_16(spec.substr(pos, end - pos)));
        pos = end + 1;
    }
}

void register_applet_overrides(void (*reg)(const char*, rc_fn)) {
    reg("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", applet::ShowKeyboard);
}

}  // namespace rt
