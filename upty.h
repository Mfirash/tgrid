#pragma once
#include <string>

void upty_init();
void fork_shell(const std::string& command_line);
void send_input(const std::string& input);
void on_async_write(uv_async_t* handle)
void on_uv_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)