/*
 * Copyright (c) 2014 Matt Fichman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, APEXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdint.h>

#ifdef _WIN32
    #define GAMESYNC_API __declspec(dllexport)
#else
    #define GAMESYNC_API
#endif

typedef uint32_t gs_Id;
typedef uint8_t gs_TypeId;
typedef double gs_Number;

typedef enum gs_SocketState {
    gs_nil,
    gs_idle,
    gs_connecting,
    gs_listening,
    gs_closed,
    gs_error,
} gs_SocketState;

typedef enum gs_SocketFlags {
    gs_write = 0x1,
    gs_read = 0x2,
} gs_SocketFlags;

#define gs_bufsize (1 << 15)

typedef struct gs_Socket {
    int sd; /* socket file descriptor */
    int status; /* socket errno code */
    gs_SocketState state; /* socket state */
    gs_SocketFlags flags;
    char write_buf[gs_bufsize];
    char* write_ptr; /* pointer to end of area user has written */
    char* write_start; /* pointer to end of area socket has read */
    char* write_checkpoint; 
    char read_buf[gs_bufsize];
    char* read_ptr; /* pointer to end of area user  has read */
    char* read_end; /* pointer to end of area socket has sent */
    char* read_checkpoint;
} gs_Socket;


/* UTILITY FUNCTIONS */
GAMESYNC_API char const* gs_strerror(int error);

/* CONNECTION MANAGEMENT */
GAMESYNC_API gs_Socket* gs_socket();
GAMESYNC_API void gs_close(gs_Socket* sd);
GAMESYNC_API void gs_connect(gs_Socket* sd, char const* addr, uint16_t port);
GAMESYNC_API void gs_listen(gs_Socket* sd, uint16_t port);
GAMESYNC_API gs_Socket* gs_accept(gs_Socket* sd);
GAMESYNC_API void gs_poll(gs_Socket** sds, int nsds, int wait);

/* CONNECTION CHECKPOINTING */
GAMESYNC_API void gs_recv_begin(gs_Socket* sd);
GAMESYNC_API int gs_recv_end(gs_Socket* sd);
GAMESYNC_API int gs_recv_ok(gs_Socket* sd, int32_t len);
GAMESYNC_API void gs_send_begin(gs_Socket* sd);
GAMESYNC_API int gs_send_end(gs_Socket* sd);
GAMESYNC_API int gs_send_ok(gs_Socket* sd, int32_t len);

/* I/O CONTROL */
GAMESYNC_API void gs_fetch(gs_Socket* sd);
GAMESYNC_API void gs_flush(gs_Socket* sd);

/* SERIALIZATION/DESERIALIZATION */
GAMESYNC_API char const* gs_recv_str(gs_Socket* sd);
GAMESYNC_API gs_TypeId gs_recv_typeid(gs_Socket* sd);
GAMESYNC_API gs_Id gs_recv_id(gs_Socket* sd);
GAMESYNC_API gs_Number gs_recv_num(gs_Socket* sd);
GAMESYNC_API int gs_send_str(gs_Socket* sd, char const* str);
GAMESYNC_API int gs_send_typeid(gs_Socket* sd, gs_TypeId id);
GAMESYNC_API int gs_send_id(gs_Socket* sd, gs_Id id);
GAMESYNC_API int gs_send_num(gs_Socket* sd, gs_Number num);

