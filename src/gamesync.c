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

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <winsock2.h>
#include <windows.h>
#undef errno
#define errno WSAGetLastError()
#define ECONNREFUSED WSAECONNREFUSED
#define EHOSTUNREACH WSAEHOSTUNREACH
#define EWOULDBLOCK WSAEWOULDBLOCK
#else
#endif

#define GAMESYNC_API __declspec(dllexport)

typedef uint32_t gs_Id;
typedef uint8_t gs_TypeId;

typedef enum gs_SocketState {
    gs_nil,
    gs_idle,
    gs_connecting,
    gs_listening,
    gs_closed,
} gs_SocketState;

typedef int gs_Flags;

int const gs_read = 0x1;
int const gs_write = 0x2;
#define gs_bufsize (1 << 15)

typedef struct gs_Socket {
    int sd; /* socket file descriptor */
    int status; /* socket errno code */
    gs_SocketState state; /* socket state */
    gs_Flags flags;
    char write_buf[gs_bufsize];
    char* write_ptr;
    char* begin;
} gs_Socket;


/* CONNECTION MANAGEMENT */

/* Creates a new socket */
static gs_Socket* gs_socket() {
    gs_Socket* sd = calloc(sizeof(gs_Socket), 1);
    WORD version = MAKEWORD(2, 2);
    WSADATA data;
    int yes = 1;
    WSAStartup(version, &data);
    sd->sd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sd->status = sd < 0 ? errno : 0;
    sd->state = gs_nil;
    sd->flags = 0;
    sd->write_ptr = sd->write_buf;
    assert(!ioctlsocket(sd->sd, FIONBIO, &yes));
    return sd;
}

/* Connects to the given addr/port, and resumes the given coroutine when the
 * connection is open */
static void gs_connect(gs_Socket* sd, char const* addr, uint16_t port) {
    struct sockaddr_in sin;
    int ret = 0;
    memset(&sin, 0, sizeof(sin));
    //inet_pton(AF_INET, addr, &sin.sin_addr);
    sin.sin_addr.s_addr = inet_addr(addr);
    sin.sin_port = htons(port);
    sin.sin_family = AF_INET;
    ret = connect(sd->sd, (struct sockaddr*)&sin, sizeof(sin));
    sd->status = ret < 0 ? errno : 0;
    sd->state = gs_connecting;

    if (sd->status == ECONNREFUSED) {
    } else if(sd->status == EHOSTUNREACH) {
    } else if(sd->status == EWOULDBLOCK) {
    } else {
        assert(!"bad return status");
    }
}

/* Closes the socket connection */
static void gs_close(gs_Socket* sd) {
    int ret = closesocket(sd->sd);
    sd->status = ret < 0 ? errno : 0;
    sd->state = gs_closed;
    free(sd);
}

/* Sets the listen port for the socket */
static void gs_listen(gs_Socket* sd, uint16_t port) {
    int const backlog = 10;
    int ret = 0;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);
    sin.sin_family = AF_INET;

    ret = bind(sd->sd, (struct sockaddr*)&sin, sizeof(sin));
    sd->status = ret < 0 ? errno : 0;
    if (ret < 0) {
        return;
    }

    ret = listen(sd->sd, backlog);
    sd->status = ret < 0 ? errno : 0;
    sd->state = gs_listening;
}

/* Accepts incoming connections */
static gs_Socket* gs_accept(gs_Socket* sd) {
    gs_Socket* ret = calloc(sizeof(gs_Socket), 1); 
    int yes = 1;
    ret->sd = accept(sd->sd, 0, 0);
    ret->status = sd->sd < 0 ? errno : 0;
    ret->state = gs_idle;
    ret->write_ptr = ret->write_buf;
    assert(!ioctlsocket(ret->sd, FIONBIO, &yes));
    return ret;
}

/* SERIALIZATION/DESERIALIZATION */

static void gs_begin(gs_Socket* sd) {
    sd->begin = sd->write_ptr;
}

static int gs_commit(gs_Socket* sd) {
    if (sd->begin) {
        printf("committed %d bytes\n", sd->write_ptr-sd->begin);
        sd->begin = 0;
        return 1; // ok, committed
    } else {
        return 0;
    }
}

static int gs_reserve(gs_Socket* sd, uint32_t len) {
    if (!sd->begin) {
        return 0; /* aborted previously, or forgot to call begin() */
    } else if ((sd->write_buf + sizeof(sd->write_buf) - sd->write_ptr) < len) {
        sd->write_ptr = sd->begin; 
        sd->begin = 0;
        return 0; /* not enough space */
    } else {
        return 1; /* good to go */
    }
}

static int gs_send_int(gs_Socket* sd, int32_t num) {
    if (!gs_reserve(sd, sizeof(num))) {
        return 0;
    }
    num = htonl(num);
    memcpy(sd->write_ptr, &num, sizeof(num));
    sd->write_ptr += sizeof(num);
    return 1;
}

static int gs_send_double(gs_Socket* sd, double num) {
    if (!gs_reserve(sd, sizeof(num))) {
        return 0;
    }
    memcpy(sd->write_ptr, &num, sizeof(num));
    sd->write_ptr += sizeof(num);
    return 1;
}

static int gs_send_str(gs_Socket* sd, char const* str) {
    int32_t len = strlen(str);
    if (!gs_send_int(sd, len)) {
        return 0;
    }
    if (!gs_reserve(sd, len)) {
        return 0;
    }
    memcpy(sd->write_ptr, str, len);
    sd->write_ptr += sizeof(len);
    return 1;
}

static int gs_send_typeid(gs_Socket* sd, gs_TypeId id) {
    if (!gs_reserve(sd, sizeof(id))) {
        return 0;
    }
    memcpy(sd->write_ptr, &id, sizeof(id));
    sd->write_ptr += sizeof(id); 
    return 1;
}

static int gs_send_id(gs_Socket* sd, gs_Id id) {
    return gs_send_int(sd, id);
}

static int gs_send_num(gs_Socket* sd, lua_Number num) {
    lua_Number whole = 0;
    lua_Number fract = modf(num, &whole);
    if (fract) {
        return gs_send_double(sd, num);     
    } else {
        return gs_send_int(sd, (int32_t)num);
    }
}

/* LUA BINDINGS */

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
    fd_set rdfds;
    fd_set wrfds;
    fd_set exfds;
    int nfds = 0;
    FD_ZERO(&rdfds);
    FD_ZERO(&wrfds);
    FD_ZERO(&exfds);

    lua_pushnil(env);
    while (lua_next(env, 1) != 0) {
        gs_Socket* sd = 0;
        lua_pushstring(env, "sd");
        lua_gettable(env, -2);
        sd = lua_touserdata(env, -1);
        sd->flags = 0;
        lua_pop(env, 2);
        FD_SET(sd->sd, &rdfds);
        FD_SET(sd->sd, &exfds);
        nfds = max(nfds, sd->sd);
        if (sd->write_ptr != sd->write_buf) {
            FD_SET(sd->sd, &wrfds);
        } else if (sd->state == gs_connecting) {
            FD_SET(sd->sd, &wrfds);
        }
    }
    select(nfds+1, &rdfds, &wrfds, &exfds, 0);

    lua_pushnil(env);
    while (lua_next(env, 1) != 0) {
        gs_Socket* sd = 0;
        lua_pushstring(env, "sd");
        lua_gettable(env, -2);
        sd = lua_touserdata(env, -1);
        lua_pop(env, 2);
        if (FD_ISSET(sd->sd, &wrfds)) {
            sd->flags |= gs_write;
        } else if (FD_ISSET(sd->sd, &exfds)) {
            sd->status = 1;
        } else if (FD_ISSET(sd->sd, &rdfds)) {
            sd->flags |= gs_read; 
        }
    }

    lua_settop(env, 0);
    return 0;
}

static int gs_Lbegin(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    gs_begin(sd);
    lua_settop(env, 0);
    return 0;
}

static int gs_Lcommit(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    int ret = gs_commit(sd);
    lua_settop(env, 0);
    lua_pushboolean(env, ret);
    return 1;
}

static int gs_Labort(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    gs_abort(sd);
    lua_settop(env, 0);
    return 0;
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

static int gs_Lstrerror(lua_State* env) {
    gs_Socket* sd = lua_touserdata(env, 1);
    char msg[1024];
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM;
    lua_settop(env, 0);
    if (FormatMessage(flags, 0, sd->status, 0, msg, sizeof(msg), 0)) {
        lua_pushstring(env, msg);
        return 1;
    } else {
        return 0;
    }
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
    char id = lua_tostring(env, 2)[0];
    assert(id == 'n' || id == 's' || id == 'b' || id == 't');
    gs_send_typeid(sd, (gs_TypeId)id);
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
    { "begin", gs_Lbegin },
    { "commit", gs_Lcommit },
    { "status", gs_Lstatus },
    { "writable", gs_Lwritable },
    { "readable", gs_Lreadable },
    { "state", gs_Lstate },
    { "strerror", gs_Lstrerror },
    { "send_str", gs_Lsend_str },
    { "send_typeid", gs_Lsend_typeid },
    { "send_id", gs_Lsend_id },
    { "send_num", gs_Lsend_num },
    { 0, 0 },
};

GAMESYNC_API luaopen_gamesyncnative(lua_State *env) {
    lua_newtable(env);
    luaL_register(env, 0, gamesync);
    return 1;
}

