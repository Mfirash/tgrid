#include "tgrid.h"
#include <thread>
#include <iostream>
#include <algorithm>
#include <string>
#include <vector>
#include <mutex>
#include <io.h>
#include <fcntl.h>
#include <winpty.h>
#include <windows.h>
#include <uv.h>

static uv_loop_t* g_loop = nullptr;
static uv_pipe_t g_stdin_pipe;
static uv_pipe_t g_stdout_pipe;
static uv_async_t g_async_write_handle;
static winpty_t* g_winpty_instance = nullptr;
static std::mutex g_input_mutex;
static std::vector<char> g_input_buffer;
void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
void on_uv_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
void on_async_write(uv_async_t* handle);

void upty_init() {
    if (!g_loop) {
        g_loop = uv_default_loop();
        uv_async_init(g_loop, &g_async_write_handle, on_async_write);
    }
}

void fork_shell(const std::string& command_line) {
    upty_init();

    std::thread([command_line]() {
        winpty_error_ptr_t err = nullptr;
        winpty_config_t* config = winpty_config_new(0, &err);
        if (!config) return;
        winpty_config_set_initial_size(config, 80, 24);
        g_winpty_instance = winpty_open(config, &err);
        winpty_config_free(config);
        if (!g_winpty_instance) return;
        LPCWSTR conin_name = winpty_conin_name(g_winpty_instance);
        LPCWSTR conout_name = winpty_conout_name(g_winpty_instance);
        uv_pipe_init(g_loop, &g_stdin_pipe, 0);
        uv_pipe_init(g_loop, &g_stdout_pipe, 0);
        HANDLE hIn = CreateFileW(conin_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        HANDLE hOut = CreateFileW(conout_name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        int stdin_fd = _open_osfhandle((intptr_t)hIn, _O_WRONLY);
        int stdout_fd = _open_osfhandle((intptr_t)hOut, _O_RDONLY);
        uv_pipe_open(&g_stdin_pipe, stdin_fd);
        uv_pipe_open(&g_stdout_pipe, stdout_fd);
        int wchars_num = MultiByteToWideChar(CP_UTF8, 0, command_line.c_str(), -1, NULL, 0);
        std::wstring wide_cmd(wchars_num, 0);
        MultiByteToWideChar(CP_UTF8, 0, command_line.c_str(), -1, &wide_cmd[0], wchars_num);
        winpty_spawn_config_t* spawn_config = winpty_spawn_config_new(
            WINPTY_SPAWN_FLAG_AUTO_SHUTDOWN,
            wide_cmd.c_str(),
            NULL, NULL, NULL, &err
        );
        winpty_spawn(g_winpty_instance, spawn_config, NULL, NULL, NULL, &err);
        winpty_spawn_config_free(spawn_config);
        uv_read_start((uv_stream_t*)&g_stdout_pipe, alloc_buffer, on_uv_read);
        uv_run(g_loop, UV_RUN_DEFAULT);
    }).detach();
}
void send_input(const std::string& input) {
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_input_buffer.insert(g_input_buffer.end(), input.begin(), input.end());
    }
    uv_async_send(&g_async_write_handle);
}

void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = (unsigned int)suggested_size;
}

void on_uv_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread > 0) {
        ioterm_write(std::string(buf->base, nread));
    } else if (nread < 0) {
        uv_read_stop(stream);
    }
    delete[] buf->base;
}

void on_async_write(uv_async_t* handle) {
    std::vector<char> to_write;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        if (g_input_buffer.empty()) return;
        to_write.swap(g_input_buffer);
    }

    char* raw_buf = new char[to_write.size()];
    std::copy(to_write.begin(), to_write.end(), raw_buf);
    uv_buf_t buf = uv_buf_init(raw_buf, (unsigned int)to_write.size());

    uv_write_t* req = new uv_write_t();
    req->data = raw_buf;
    uv_write(req, (uv_stream_t*)&g_stdin_pipe, &buf, 1, [](uv_write_t* req, int status) {
        delete[] static_cast<char*>(req->data);
        delete req;
    });
}