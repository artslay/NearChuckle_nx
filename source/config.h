#pragma once

struct Config {
    char data_root[256];
    char lib_dir[256];
    char mesa_driver[64];
    int screen_width;
    int screen_height;
    int fov;
    int vsync;
    int disable_shader_compilation;
};

extern Config config;

int read_config(const char* path);
