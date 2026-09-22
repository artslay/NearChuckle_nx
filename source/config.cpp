#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

Config config;

static void trim(char* s) {
    if (!s) return;

    char* begin = s;
    while (*begin && std::isspace(static_cast<unsigned char>(*begin)))
        ++begin;

    if (begin != s)
        std::memmove(s, begin, std::strlen(begin) + 1);

    size_t len = std::strlen(s);
    while (len > 0 && std::isspace(static_cast<unsigned char>(s[len - 1])))
        s[--len] = '\0';
}

static void set_string(char* dst, size_t dst_size, const char* value) {
    if (!dst || dst_size == 0) return;
    std::strncpy(dst, value ? value : "", dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void parse_line(char* line) {
    trim(line);
    if (!line[0] || line[0] == '#')
        return;

    char* sep = line;
    while (*sep && !std::isspace(static_cast<unsigned char>(*sep)))
        ++sep;

    if (!*sep)
        return;

    *sep++ = '\0';
    while (*sep && std::isspace(static_cast<unsigned char>(*sep)))
        ++sep;

    char* comment = std::strchr(sep, '#');
    if (comment)
        *comment = '\0';

    trim(sep);

    if (!std::strcmp(line, "data_root")) {
        set_string(config.data_root, sizeof(config.data_root), sep);
    } else if (!std::strcmp(line, "lib_dir")) {
        set_string(config.lib_dir, sizeof(config.lib_dir), sep);
    } else if (!std::strcmp(line, "mesa_driver")) {
        set_string(config.mesa_driver, sizeof(config.mesa_driver), sep);
    } else if (!std::strcmp(line, "screen_width")) {
        config.screen_width = std::atoi(sep);
    } else if (!std::strcmp(line, "screen_height")) {
        config.screen_height = std::atoi(sep);
    } else if (!std::strcmp(line, "fov")) {
        config.fov = std::atoi(sep);
    } else if (!std::strcmp(line, "vsync")) {
        config.vsync = std::atoi(sep);
    }
}

int read_config(const char* path) {
    std::memset(&config, 0, sizeof(config));

    set_string(config.data_root, sizeof(config.data_root), "/switch/NearChuckle_nx/game");
    set_string(config.lib_dir, sizeof(config.lib_dir), "/switch/NearChuckle_nx/game/lib");
    set_string(config.mesa_driver, sizeof(config.mesa_driver), "zink");

    config.screen_width = 1280;
    config.screen_height = 720;
    config.fov = 90;
    config.vsync = 1;

    FILE* f = std::fopen(path, "r");
    if (!f)
        return -1;

    char line[1024];
    while (std::fgets(line, sizeof(line), f))
        parse_line(line);

    std::fclose(f);
    return 0;
}
