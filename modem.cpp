// AT-Modem Emulator — C++20, Linux
// Компилировать: g++ -std=c++20 -O2 -o modem modem.cpp
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cctype>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

struct Entry { std::string pattern, answer; };

// ─── Кастомный матчер (glob-стиль) ─────────────────────────────────
// '.'   — любой один символ
// '*'   — ноль или любая цепочка символов
// '[…]' — один из перечисленных символов
// Буквы — регистронезависимо
bool is_match(std::string_view t, std::string_view p) {
    if (p.empty()) return t.empty();

    if (p[0] == '*') {                              // «звёздочка»
        p.remove_prefix(1);
        for (size_t i = 0; i <= t.size(); ++i)
            if (is_match(t.substr(i), p)) return true;
        return false;
    }
    if (t.empty()) return false;                    // паттерн не пуст, а текст — пуст

    if (p[0] == '.') {                              // «точка» — любой символ
        return is_match(t.substr(1), p.substr(1));
    }
    if (p[0] == '[') {                              // «класс символов»
        auto close = p.find(']');
        if (close == std::string_view::npos) return false;
        bool found = false;
        for (char ch : p.substr(1, close - 1))
            if (std::toupper(ch) == std::toupper(t[0])) { found = true; break; }
        return found && is_match(t.substr(1), p.substr(close + 1));
    }
    // обычный символ — сравниваем без учёта регистра
    return std::toupper(p[0]) == std::toupper(t[0])
        && is_match(t.substr(1), p.substr(1));
}

// ─── Загрузка словаря ──────────────────────────────────────────────
auto load_dict(const char* path) {
    std::vector<Entry> dict;
    std::ifstream file(path);
    if (!file) { std::cerr << "dict: не удалось открыть " << path << '\n'; return dict; }

    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#') continue;
        if (auto pos = line.find('='); pos != std::string::npos)
            dict.push_back({line.substr(0, pos), line.substr(pos + 1)});
    }
    std::cerr << "dict: загружено " << dict.size() << " записей\n";
    return dict;
}

// ─── main ──────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Использование: modem <tty-порт> <dict-файл>\n"
                     "Пример:        modem /dev/pts/3 dict.txt\n";
        return 1;
    }

    auto dict = load_dict(argv[2]);

    int fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open"); return 1; }

    termios tio{};
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);                               // raw-режим: без буферизации и обработки
    tcsetattr(fd, TCSANOW, &tio);

    bool echo = true;
    std::string buf;
    char ch;

    std::cerr << "modem: слушаю " << argv[1] << "  (echo=" << echo << ")\n";

    while (read(fd, &ch, 1) == 1) {
        if (echo) write(fd, &ch, 1);               // эхо символа обратно
        if (ch != '\r') { buf += ch; continue; }   // копим до Enter

        // ── Обработка ATE0 / ATE1 ──
        if (is_match(buf, "ATE0")) echo = false;
        if (is_match(buf, "ATE1")) echo = true;

        // ── Поиск ответа в словаре ──
        std::string resp = "ERROR";
        for (const auto& [pat, ans] : dict)
            if (is_match(buf, pat)) { resp = ans; break; }

        std::string frame = "\r\n" + resp + "\r\n"; // формат ответа модема
        write(fd, frame.data(), frame.size());
        buf.clear();
    }
    close(fd);
}

