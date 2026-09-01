/*
 * AT-Modem Emulator — C++17, Linux, POSIX
 *
 * Build:  g++ -std=c++17 -O2 -Wall -o modem main.cpp
 * Usage:  ./modem [tty] [rules]
 *           tty   — TTY device    (default: /dev/pts/2)
 *           rules — rules file    (default: rules.txt)
 */
#include <fcntl.h>          // open, O_RDWR, O_NOCTTY
#include <termios.h>        // termios, cfset*speed, tcsetattr
#include <unistd.h>         // read, write, close
#include <cctype>           // toupper
#include <cstring>          // strchr
#include <fstream>          // ifstream
#include <iostream>         // cerr
#include <string>
#include <vector>

struct Rule { std::string pattern, answer; };

/* ── Замена литеральных \r \n на настоящие CR/LF ────────────────── */
static std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'r') { out += '\r'; ++i; continue; }
            if (s[i + 1] == 'n') { out += '\n'; ++i; continue; }
        }
        out += s[i];
    }
    return out;
}

/* ── Загрузка правил из файла (expect=answer) ───────────────────── */
static std::vector<Rule> load_rules(const char* path) {
    std::vector<Rule> rules;
    std::ifstream f(path);
    if (!f) { std::cerr << "ERR: cannot open " << path << '\n'; return rules; }
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq != std::string::npos)
            rules.push_back({line.substr(0, eq), unescape(line.substr(eq + 1))});
    }
    std::cerr << "[rules] loaded " << rules.size() << " entries from " << path << '\n';
    return rules;
}

/* ── Кастомный матчер шаблонов ──────────────────────────────────── *
 *  '.'    — ровно один любой символ                                 *
 *  '*'    — ноль или более любых символов                           *
 *  '[…]'  — один из перечисленных внутри скобок                     *
 *  Буквы  — сравниваются без учёта регистра (case-insensitive)      */
static bool is_match(const char* t, const char* p) {
    if (*p == '\0') return *t == '\0';

    if (*p == '*') {                                    // «звёздочка»
        for (const char* s = t; ; ++s) {                //   перебираем все суффиксы текста
            if (is_match(s, p + 1)) return true;
            if (*s == '\0') return false;
        }
    }
    if (*t == '\0') return false;                        // текст кончился раньше паттерна

    if (*p == '.') return is_match(t + 1, p + 1);       // «точка» — любой один символ

    if (*p == '[') {                                     // «класс символов»
        const char* cl = strchr(p, ']');
        if (!cl) return false;                           //   незакрытая скобка — ошибка
        bool hit = false;
        for (const char* c = p + 1; c < cl; ++c)
            if (toupper((unsigned char)*c) == toupper((unsigned char)*t))
                { hit = true; break; }
        return hit && is_match(t + 1, cl + 1);
    }
    /* Обычный символ — регистронезависимое сравнение */
    return toupper((unsigned char)*p) == toupper((unsigned char)*t)
        && is_match(t + 1, p + 1);
}

/* ── Настройка TTY: raw-режим, 115200 8N1 ──────────────────────── */
static int setup_tty(const char* dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY);              // без привязки к controlling terminal
    if (fd < 0) { perror("open"); return -1; }

    struct termios tio {};
    tcgetattr(fd, &tio);

    /* Входные флаги: отключаем всю предобработку */
    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                      INLCR | IGNCR | ICRNL | IXON);

    /* Выходные: без постобработки */
    tio.c_oflag &= ~OPOST;

    /* Управляющие: 8 бит данных (CS8), без чётности, 1 стоп-бит */
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= CS8 | CREAD | CLOCAL;                // CREAD — приём вкл., CLOCAL — локальная линия

    /* Локальные: отключаем канонический режим, эхо, сигналы */
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    tio.c_cc[VMIN]  = 1;                                // блокирующее чтение: мин. 1 байт
    tio.c_cc[VTIME] = 0;                                // без таймаута

    cfsetispeed(&tio, B115200);                          // скорость порта: 115200 бод
    cfsetospeed(&tio, B115200);

    tcsetattr(fd, TCSANOW, &tio);                       // применить немедленно
    return fd;
}

/* ── Точка входа ────────────────────────────────────────────────── */
int main(int argc, char* argv[]) {
    const char* tty_path   = (argc > 1) ? argv[1] : "/dev/pts/2";
    const char* rules_path = (argc > 2) ? argv[2] : "rules.txt";

    auto rules = load_rules(rules_path);
    int fd = setup_tty(tty_path);
    if (fd < 0) return 1;

    std::cerr << "[modem] listening on " << tty_path << " (115200 8N1)\n";

    std::string buf;
    char ch;

    while (read(fd, &ch, 1) == 1) {
        if (ch != '\r' && ch != '\n') {                  // копим байты до разделителя
            buf += ch;
            continue;
        }
        if (buf.empty()) continue;                       // пустая строка — пропускаем

        /* Ищем первое совпавшее правило */
        std::string resp = "ERROR";
        for (const auto& r : rules)
            if (is_match(buf.c_str(), r.pattern.c_str())) { resp = r.answer; break; }

        /* Отправляем ответ в формате модема: \r\n<ответ>\r\n */
        std::string frame = "\r\n" + resp + "\r\n";
        write(fd, frame.data(), frame.size());

        buf.clear();
    }
    close(fd);
}

