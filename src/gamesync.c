/*
 * Copyright (c) 2013 Matt Fichman
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

#include "gamesync.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>

#ifdef _WIN32
    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define VC_EXTRALEAN
    #include <winsock2.h>
    #include <windows.h>
    #undef max // Windows is lame
    #undef errno // Arguably stupid
    #define errno WSAGetLastError()
    #define ECONNREFUSED WSAECONNREFUSED
    #define EHOSTUNREACH WSAEHOSTUNREACH
    #define EWOULDBLOCK WSAEWOULDBLOCK
    #define EADDRINUSE WSAEADDRINUSE
    #define EINPROGRESS WSAEINPROGRESS
    #define EOK ERROR_SUCCESS
    #define close closesocket
#else
    #define EOK 0
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <errno.h>
    #include <string.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif


/* UTILTY FUNCTIONS */

#define max(x,y) ((x)>(y)?(x):(y))

/* Set common socket flags/connection control options */
static void gs_setflags(gs_Socket* sd) {
#ifdef _WIN32
    u_long yes = 1;
    assert(!ioctlsocket(sd->sd, FIONBIO, &yes));
#else
	int const flags = fcntl(sd->sd, F_GETFL, 0);
	assert(!fcntl(sd->sd, F_SETFL, flags | O_NONBLOCK));
#endif
}

/* Returns an error string for the given system-specific error code */
char const* gs_strerror(int error) {
#ifdef _WIN32
    static char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    LPSTR buf = (LPSTR)buffer;
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM;
    FormatMessage(flags, 0, error, 0, buf, sizeof(buffer)-1, 0);
	return buf;
#else
	return strerror(error);
#endif
}

/* CONNECTION MANAGEMENT */

/* Creates a new socket */
gs_Socket* gs_socket() {
    gs_Socket* sd = calloc(sizeof(gs_Socket), 1);
#ifdef _WIN32
    WORD version = MAKEWORD(2, 2);
    WSADATA data;
    WSAStartup(version, &data);
#endif
    sd->sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sd->status = sd < 0 ? errno : 0;
    sd->state = gs_nil;
    sd->flags = 0;
    sd->write_ptr = sd->write_buf;
    sd->write_start = sd->write_buf;
    sd->read_ptr = sd->read_buf;
    sd->read_end = sd->read_buf;
    assert(!sd->status);
	gs_setflags(sd);
    return sd;
}

/* Closes the socket connection */
void gs_close(gs_Socket* sd) {
    int const ret = close(sd->sd);
    sd->status = ret < 0 ? errno : 0;
    sd->state = gs_closed;
    assert(!sd->status);
    free(sd);
}

/* Connects to the given addr/port, and resumes the given coroutine when the
 * connection is open */
void gs_connect(gs_Socket* sd, char const* addr, uint16_t port) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    //inet_pton(AF_INET, addr, &sin.sin_addr);
    sin.sin_addr.s_addr = inet_addr(addr);
    sin.sin_port = htons(port);
    sin.sin_family = AF_INET;

    int const ret = connect(sd->sd, (struct sockaddr*)&sin, sizeof(sin));
    sd->status = ret < 0 ? errno : 0;

    switch (sd->status) {
    case ECONNREFUSED: sd->state = gs_error; break;
    case EHOSTUNREACH: sd->state = gs_error; break;
    case EWOULDBLOCK: sd->state = gs_connecting; break;
	case EINPROGRESS: sd->state = gs_connecting; break;
    default:
		printf("%s\n", gs_strerror(sd->status));
        assert(!"bad return status");
        break;
    }
}

/* Sets the listen port for the socket */
void gs_listen(gs_Socket* sd, uint16_t port) {
    int const backlog = 10;
    int ret = 0;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);
    sin.sin_family = AF_INET;

    ret = bind(sd->sd, (struct sockaddr*)&sin, sizeof(sin));
    sd->status = ret < 0 ? errno : 0;

    switch (sd->status) {
    case EADDRINUSE: sd->state = gs_error; return;
    case EOK: break; 
    default:
        assert(!"bad return status");
    }

    ret = listen(sd->sd, backlog);
    sd->status = ret < 0 ? errno : 0;
    sd->state = gs_listening;
    assert(!sd->status);
}

/* Accepts incoming connections */
gs_Socket* gs_accept(gs_Socket* sd) {
    gs_Socket* ret = calloc(sizeof(gs_Socket), 1); 
    ret->sd = accept(sd->sd, 0, 0);
    ret->status = sd->sd < 0 ? errno : 0;
    ret->state = gs_idle;
    ret->write_ptr = ret->write_buf;
    ret->write_start = ret->write_buf;
    ret->read_ptr = ret->read_buf;
    ret->read_end = ret->read_buf;
	gs_setflags(sd);
    return ret;
}

/* Poll for readable/writable sockets in the socket array. If 'wait' is true,
 * then wait until at least one socket is readable/writable. 
 */
void gs_poll(gs_Socket** sds, int nsds, int wait) {
    struct timeval tv = { 0, 0 };
    fd_set rdfds, wrfds, exfds;
    int nfds = 0;
    FD_ZERO(&rdfds);
    FD_ZERO(&wrfds);
    FD_ZERO(&exfds);

    /* Set up FD_SETs. */
    for (int i = 0; i < nsds; ++i) {
        gs_Socket* const sd = sds[i];
        sd->flags = 0;
        nfds = max(nfds, sd->sd);
        if (sd->state != gs_error) {
            FD_SET(sd->sd, &rdfds);
            FD_SET(sd->sd, &exfds);
        }
        if (sd->state == gs_error) {
            // Skip 
        } else if (sd->write_ptr != sd->write_buf) {
            FD_SET(sd->sd, &wrfds);
        } else if (sd->state == gs_connecting) {
            FD_SET(sd->sd, &wrfds);
        }
    }

    select(nfds+1, &rdfds, &wrfds, &exfds, wait ? 0 : &tv);

    for (int i = 0; i < nsds; ++i) {
        gs_Socket* const sd = sds[i];
        if (FD_ISSET(sd->sd, &wrfds)) {
            sd->flags |= gs_write;
			if (sd->state == gs_connecting) {
				sd->state = gs_idle;
			}
        } else if (FD_ISSET(sd->sd, &exfds)) {
            sd->status = 1;
        } else if (FD_ISSET(sd->sd, &rdfds)) {
            sd->flags |= gs_read; 
        }
    }
} 

/* CONNECTION CHECKPOINTING */

/* Begin receiving message */
void gs_recv_begin(gs_Socket* sd) {
    sd->read_checkpoint = sd->read_ptr;
}

/* Return true if the whole message was received */
int gs_recv_end(gs_Socket* sd) {
    if (sd->read_checkpoint) {
        sd->read_checkpoint = 0;
        if (sd->read_ptr == sd->read_end) {
            sd->read_ptr = sd->read_buf;
            sd->read_end = sd->read_buf;
        }
        return 1; // ok, committed
    } else {
        sd->read_checkpoint = 0;
        return 0;
    }
}

/* Checks to see if 'len' bytes are available to read in the read buffer */
int gs_recv_ok(gs_Socket* sd, int32_t len) {
    if (!sd->read_checkpoint) {
        return 0; /* aborted previously on read */
    } else if ((sd->read_end - sd->read_ptr) < len) {
        sd->read_ptr = sd->read_checkpoint;
        sd->read_checkpoint = 0;
        return 0; /* not enough data in the buffer */
    } else {
        return 1; /* good to go */
    }
}

/* Begin sending message */
void gs_send_begin(gs_Socket* sd) {
    sd->write_checkpoint = sd->write_ptr;
}

/* Return true if the whole message was sent */
int gs_send_end(gs_Socket* sd) {
    if (sd->write_checkpoint) {
        sd->write_checkpoint = 0;
        return 1; // ok, committed
    } else {
        sd->write_checkpoint = 0;
        return 0;
    }
}

/* Checks to see if 'len' bytes are available in the write buffer */
int gs_send_ok(gs_Socket* sd, int32_t len) {
    if (!sd->write_checkpoint) {
        return 0; /* aborted previously, or forgot to call begin() */
    } else if ((sd->write_buf + sizeof(sd->write_buf) - sd->write_ptr) < len) {
        sd->write_ptr = sd->write_checkpoint; 
        sd->write_checkpoint = 0;
        return 0; /* not enough space */
    } else {
        return 1; /* good to go */
    }
}

/* SERIALIZATION/DESERIALIZATION */

/* Send to the remote side */
void gs_flush(gs_Socket* sd) {
    ptrdiff_t const len = sd->write_ptr - sd->write_start;
    if (!len) {
        return;
    } 
    int const ret = send(sd->sd, sd->write_start, len, 0);
    if (ret < 0) {
        sd->status = errno;
        sd->state = gs_error;
        if (!sd->write_checkpoint) {
            sd->write_ptr = sd->write_checkpoint;
            sd->write_checkpoint = 0;
        }
    } else {
        sd->write_start += len;
    }
    if (sd->write_start == sd->write_ptr) {
        sd->write_start = sd->write_buf;
        sd->write_ptr = sd->write_buf;
    }
}


int gs_send_str(gs_Socket* sd, char const* str) {
    int32_t const len = strlen(str);
    int32_t const netlen = htonl(len);
    if (!gs_send_ok(sd, sizeof(len))) {
        return 0;
    }
    memcpy(sd->write_ptr, &netlen, sizeof(len));
    sd->write_ptr += sizeof(len);
    if (!gs_send_ok(sd, len+1)) {
        return 0;
    }
    memcpy(sd->write_ptr, str, len+1);
    sd->write_ptr += len+1;
    return 1;
}

int gs_send_typeid(gs_Socket* sd, gs_TypeId id) {
    if (!gs_send_ok(sd, sizeof(id))) {
        return 0;
    }
    memcpy(sd->write_ptr, &id, sizeof(id));
    sd->write_ptr += sizeof(id); 
    return 1;
}

int gs_send_id(gs_Socket* sd, gs_Id id) {
    if (!gs_send_ok(sd, sizeof(id))) {
        return 0;
    }
    id = htonl(id);
    memcpy(sd->write_ptr, &id, sizeof(id));
    sd->write_ptr += sizeof(id);
    return 1;
}

int gs_send_num(gs_Socket* sd, gs_Number num) {
    if (!gs_send_ok(sd, sizeof(num))) {
        return 0;
    }
    memcpy(sd->write_ptr, &num, sizeof(num));
    sd->write_ptr += sizeof(num);
    return 1;
}

void gs_fetch(gs_Socket* sd) {
    ptrdiff_t len = sd->read_buf + sizeof(sd->read_buf) - sd->read_end;
    int const ret = recv(sd->sd, sd->read_end, len, 0);
    if (ret < 0) {
        sd->status = errno;
        sd->state = gs_error;
        if (!sd->read_checkpoint) {
            sd->read_ptr = sd->read_checkpoint;
            sd->read_checkpoint = 0;
        }
    } else {
        sd->read_end += ret;
    }
}

char const* gs_recv_str(gs_Socket* sd) {
    int32_t len = 0;
    char const* str = 0;
    if (!gs_recv_ok(sd, sizeof(len))) {
        return 0;
    }
    memcpy(&len, sd->read_ptr, sizeof(len));
    sd->read_ptr += sizeof(len);
    len = ntohl(len);
    if (!gs_recv_ok(sd, len+1)) {
        return 0;
    }
    str = sd->read_ptr;
    sd->read_ptr += len+1;
    return str;
}

gs_TypeId gs_recv_typeid(gs_Socket* sd) {
    gs_TypeId id = 0;
    if (!gs_recv_ok(sd, sizeof(id))) {
        return 0;
    }
    memcpy(&id, sd->read_ptr, sizeof(id));
    sd->read_ptr += sizeof(id);
    return id;
}

gs_Id gs_recv_id(gs_Socket* sd) {
    gs_Id id = 0;
    if (!gs_recv_ok(sd, sizeof(id))) {
        return 0;
    }
    memcpy(&id, sd->read_ptr, sizeof(id));
    sd->read_ptr += sizeof(id);
    return ntohl(id);
}

gs_Number gs_recv_num(gs_Socket* sd) {
    gs_Number num = 0;
    if (!gs_recv_ok(sd, sizeof(num))) {
        return 0;
    }
    memcpy(&num, sd->read_ptr, sizeof(num));
    sd->read_ptr += sizeof(num);
    return num;
}


/* LUA BINDINGS */

static int gs_Lstrerror(lua_State* env) {
	int error = lua_tointeger(env, 1);
    lua_settop(env, 0);
    lua_pushstring(env, gs_strerror(error));
	return 1;
}

static int gs_Lsocket(lua_State* env) {
    lua_settop(env, 0);
    lua_pushlightuserdata(env, gs_socket()); 
    return 1;
}

static int gs_Lconnect(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    char const* addr = lua_tostring(env, 2);
    uint16_t port = (uint16_t)lua_tonumber(env, 3);
    gs_connect(sd, addr, port);
    lua_settop(env, 0);
    return 0;
}

static int gs_Lclose(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    gs_close(sd);
    lua_settop(env, 0);
    return 0;
}

static int gs_Llisten(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    uint16_t port = (uint16_t)lua_tonumber(env, 2);
    gs_listen(sd, port);
    lua_settop(env, 0);
    return 0;
}

static int gs_Laccept(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_pushlightuserdata(env, gs_accept(sd)); 
    return 1;
}

static int gs_Lpoll(lua_State* env) {
    int const wait = lua_toboolean(env, 2);
    int nsds = 0;

    lua_pushnil(env);
	for (; lua_next(env, 1) != 0; ++nsds) {
		lua_pop(env, 1);
	}

    gs_Socket** sds = calloc(sizeof(gs_Socket*), nsds);
    lua_pushnil(env);
    for (int i = 0; lua_next(env, 1) != 0; ++i) {
        lua_pushstring(env, "sd");
        lua_gettable(env, -2);
        sds[i] = lua_touserdata(env, -1);
        lua_pop(env, 2);
    }

    gs_poll(sds, nsds, wait);
    lua_settop(env, 0);
    return 0;
}

static int gs_Lsend_begin(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    gs_send_begin(sd);
    lua_settop(env, 0);
    return 0;
}

static int gs_Lrecv_begin(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    gs_recv_begin(sd);
    lua_settop(env, 0);
    return 0;
}

static int gs_Lsend_end(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    int ret = gs_send_end(sd);
    lua_settop(env, 0);
    lua_pushboolean(env, ret);
    return 1;
}

static int gs_Lrecv_end(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    int ret = gs_recv_end(sd);
    lua_settop(env, 0);
    lua_pushboolean(env, ret);
    return 1;
}

static int gs_Lstatus(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    lua_pushnumber(env, sd->status);
    return 1;
}

static int gs_Lwritable(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    lua_pushboolean(env, sd->flags & gs_write);
    return 1;
}

static int gs_Lreadable(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    lua_pushboolean(env, sd->flags & gs_read);
    return 1;
}

static int gs_Lflush(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    gs_flush(sd);
    return 0;
}

static int gs_Lsend_str(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    char const* str = lua_tostring(env, 2);
    gs_send_str(sd, str);
    lua_settop(env, 0);
    return 0; 
}

static int gs_Lsend_typeid(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    gs_TypeId id = (gs_TypeId)lua_tonumber(env, 2);
    assert(id == 'n' || id == 's' || id == 'b' || id == 't');
    gs_send_typeid(sd, id);
    lua_settop(env, 0);
    return 0;
}

static int gs_Lsend_id(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_Number id = lua_tonumber(env, 2);
    assert(id < UINT32_MAX && id >= 0);
    gs_send_id(sd, (gs_Id)id);
    lua_settop(env, 0);
    return 0;
}

static int gs_Lsend_num(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_Number num = lua_tonumber(env, 2);
    gs_send_num(sd, num);
    lua_settop(env, 0);
    return 0;
}

static int gs_Lfetch(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    gs_fetch(sd);
    return 0;
}

static int gs_Lrecv_str(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    lua_pushstring(env, gs_recv_str(sd));
    return 1; 
}

static int gs_Lrecv_typeid(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    lua_pushnumber(env, gs_recv_typeid(sd));
    return 1;
}

static int gs_Lrecv_id(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    lua_pushnumber(env, gs_recv_id(sd));
    return 1;
}

static int gs_Lrecv_num(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    lua_pushnumber(env, gs_recv_num(sd));
    return 1;
}

static int gs_Lstate(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    lua_settop(env, 0);
    switch (sd->state) {
    case gs_nil:
        lua_pushstring(env, "nil");
        break;
    case gs_idle:
        lua_pushstring(env, "idle");
        break;
    case gs_connecting:
        lua_pushstring(env, "connecting");
        break;
    case gs_listening:
        lua_pushstring(env, "listening");
        break;
    case gs_closed:
        lua_pushstring(env, "closed");
        break;
    case gs_error:
        lua_pushstring(env, "error");
        break;
    default:
        lua_pushstring(env, "");
        break;
    }
    return 1;
}

static const luaL_reg gamesync[] = {
    { "socket", gs_Lsocket },
    { "connect", gs_Lconnect },
    { "close", gs_Lclose },
    { "listen", gs_Llisten },
    { "accept", gs_Laccept },
    { "poll", gs_Lpoll },
    { "status", gs_Lstatus },
    { "writable", gs_Lwritable },
    { "readable", gs_Lreadable },
    { "state", gs_Lstate },
    { "strerror", gs_Lstrerror },
    { "fetch", gs_Lfetch },
    { "flush", gs_Lflush },
    { "send_begin", gs_Lsend_begin },
    { "send_end", gs_Lsend_end },
    { "send_str", gs_Lsend_str },
    { "send_typeid", gs_Lsend_typeid },
    { "send_id", gs_Lsend_id },
    { "send_num", gs_Lsend_num },
    { "recv_begin", gs_Lrecv_begin },
    { "recv_end", gs_Lrecv_end },
    { "recv_str", gs_Lrecv_str },
    { "recv_typeid", gs_Lrecv_typeid },
    { "recv_id", gs_Lrecv_id },
    { "recv_num", gs_Lrecv_num },
    { 0, 0 },
};

GAMESYNC_API int luaopen_lib_gamesync(lua_State *env) {
    lua_newtable(env);
    luaL_register(env, 0, gamesync);
    return 1;
}

