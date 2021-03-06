/*=========================================================================*\
* Input/Output interface for Lua programs
* LuaSocket toolkit
*
* RCS ID: $Id: buffer.c,v 1.28 2007/06/11 23:44:54 diego Exp $
\*=========================================================================*/
#include "lua.h"
#include "lauxlib.h"

#include "buffer.h"
#include "stream.h"

/*=========================================================================*\
* Internal function prototypes
\*=========================================================================*/
static int recvraw(p_buffer buf, size_t wanted, luaL_Buffer *b);
static int recvline(p_buffer buf, luaL_Buffer *b);
static int recvall(p_buffer buf, luaL_Buffer *b);
static int buffer_get(p_buffer buf, const char **data, size_t *count);
static void buffer_skip(p_buffer buf, size_t count);
static int sendraw(p_buffer buf, const char *data, size_t count, size_t *sent);
static int recvistream(lua_State *L, p_buffer buf, size_t *size);
static int sendostream(lua_State *L, p_buffer buf, size_t *sent);

/* min and max macros */
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? x : y)
#endif
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? x : y)
#endif

struct t_userdata_ {
	lua_Stream *ostream;
	lua_Stream *istream;
};

/*=========================================================================*\
* Exported functions
\*=========================================================================*/
/*-------------------------------------------------------------------------*\
* Initializes module
\*-------------------------------------------------------------------------*/
int buffer_open(lua_State *L) {
    (void) L;
    return 0;
}

/*-------------------------------------------------------------------------*\
* Initializes C structure 
\*-------------------------------------------------------------------------*/
void buffer_init(p_buffer buf, p_io io, p_timeout tm) {
	t_userdata* userdata;
	buf->first = buf->last = 0;
    buf->io = io;
    buf->tm = tm;
    buf->received = buf->sent = 0;
    buf->birthday = timeout_gettime();
	
	userdata = (t_userdata*)malloc(sizeof(t_userdata));
	userdata->ostream = NULL;
	userdata->istream = NULL;
	buf->userdata = userdata;
}

/*-------------------------------------------------------------------------*\
* object:getstats() interface
\*-------------------------------------------------------------------------*/
int buffer_close(lua_State *L, p_buffer buf) {
	if (buf->userdata)
	{
		t_userdata* userdata = buf->userdata;
		if (userdata->istream)
			stream_unref(L, userdata->istream);
		if (userdata->ostream)
			stream_unref(L, userdata->ostream);
		free(userdata);
		buf->userdata = NULL;
	}
	return 1;
}

int buffer_checkclosed(lua_State *L, p_buffer buf) {
	if (!buf->userdata)
		luaL_error(L, "socket closed");
	return 1;
}

int buffer_meth_getstats(lua_State *L, p_buffer buf) {
	t_userdata* userdata = buf->userdata;
    lua_pushnumber(L, buf->received);
    lua_pushnumber(L, buf->sent);
    lua_pushnumber(L, timeout_gettime() - buf->birthday);
    lua_pushnumber(L, stream_size(userdata->istream));
    lua_pushnumber(L, stream_size(userdata->ostream));
    return 5;
}

/*-------------------------------------------------------------------------*\
* object:setstats() interface
\*-------------------------------------------------------------------------*/
int buffer_meth_setstats(lua_State *L, p_buffer buf) {
	t_userdata* userdata = buf->userdata;
    buf->received = (long) luaL_optnumber(L, 2, buf->received); 
    buf->sent = (long) luaL_optnumber(L, 3, buf->sent); 
    if (lua_isnumber(L, 4)) buf->birthday = timeout_gettime() - lua_tonumber(L, 4);
    lua_pushnumber(L, 1);
    return 1;
}

/*-------------------------------------------------------------------------*\
* object:send() interface
\*-------------------------------------------------------------------------*/
int buffer_meth_send(lua_State *L, p_buffer buf) {
    int top = lua_gettop(L);
    int err = IO_DONE;
    size_t size = 0, sent = 0;
	const char *data;
	long start = 1, end = -1;
    p_timeout tm = timeout_markstart(buf->tm);
	if (top == 1) {
		err = sendostream(L, buf, &sent);
		goto end;
	}
	data = luaL_checklstring(L, 2, &size);
	start = (long) luaL_optnumber(L, 3, 1);
	end = (long) luaL_optnumber(L, 4, -1);
	
	if (start < 0) start = (long) (size+start+1);
	if (end < 0) end = (long) (size+end+1);
	if (start < 1) start = (long) 1;
	if (end > (long) size) end = (long) size;
	if (start <= end) err = sendraw(buf, data+start-1, end-start+1, &sent);
end:
    /* check if there was an error */
    if (err != IO_DONE) {
        lua_pushnil(L);
        lua_pushstring(L, buf->io->error(buf->io->ctx, err)); 
        lua_pushnumber(L, sent+start-1);
    } else {
        lua_pushnumber(L, sent+start-1);
        lua_pushnil(L);
        lua_pushnil(L);
    }
#ifdef LUASOCKET_DEBUG
    /* push time elapsed during operation as the last return value */
    lua_pushnumber(L, timeout_gettime() - timeout_getstart(tm));
#endif
    return lua_gettop(L) - top;
}

/*-------------------------------------------------------------------------*\
* object:receive() interface
\*-------------------------------------------------------------------------*/
int buffer_meth_receive(lua_State *L, p_buffer buf) {
    int err = IO_DONE, top = lua_gettop(L);
    luaL_Buffer b;
    size_t size;
    const char *part = luaL_optlstring(L, 3, "", &size);
    p_timeout tm = timeout_markstart(buf->tm);
    if (top == 1) {
		err = recvistream(L, buf, &size);
		if (err != IO_DONE) {
			lua_pushnil(L);
			lua_pushstring(L, buf->io->error(buf->io->ctx, err));
			lua_pushnil(L);
		} else {
			lua_pushnumber(L, size);
			lua_pushnil(L);
			lua_pushnil(L);
		}
		goto end;
    }
    /* initialize buffer with optional extra prefix 
     * (useful for concatenating previous partial results) */
    luaL_buffinit(L, &b);
    luaL_addlstring(&b, part, size);
    /* receive new patterns */
	if (!lua_isnumber(L, 2)) {
        const char *p= luaL_optstring(L, 2, "*l");
        if (p[0] == '*' && p[1] == 'l') err = recvline(buf, &b);
        else if (p[0] == '*' && p[1] == 'a') err = recvall(buf, &b); 
        else luaL_argcheck(L, 0, 2, "invalid receive pattern");
        /* get a fixed number of bytes (minus what was already partially 
         * received) */
    } else err = recvraw(buf, (size_t) lua_tonumber(L, 2)-size, &b);
    /* check if there was an error */
    if (err != IO_DONE) {
        /* we can't push anyting in the stack before pushing the
         * contents of the buffer. this is the reason for the complication */
        luaL_pushresult(&b);
        lua_pushstring(L, buf->io->error(buf->io->ctx, err)); 
        lua_pushvalue(L, -2); 
        lua_pushnil(L);
        lua_replace(L, -4);
    } else {
        luaL_pushresult(&b);
        lua_pushnil(L);
        lua_pushnil(L);
    }
end:
#ifdef LUASOCKET_DEBUG
    /* push time elapsed during operation as the last return value */
    lua_pushnumber(L, timeout_gettime() - timeout_getstart(tm));
#endif
    return lua_gettop(L) - top;
}

/*-------------------------------------------------------------------------*\
* Determines if there is any data in the read buffer
\*-------------------------------------------------------------------------*/
int buffer_isempty(p_buffer buf) {
    return buf->first >= buf->last;
}

/*=========================================================================*\
* Internal functions
\*=========================================================================*/
/*-------------------------------------------------------------------------*\
* Sends a block of data (unbuffered)
\*-------------------------------------------------------------------------*/
#define STEPSIZE 8192
static int sendraw(p_buffer buf, const char *data, size_t count, size_t *sent) {
    p_io io = buf->io;
    p_timeout tm = buf->tm;
    size_t total = 0;
    int err = IO_DONE;
    while (total < count && err == IO_DONE) {
        size_t done;
        size_t step = (count-total <= STEPSIZE)? count-total: STEPSIZE;
        err = io->send(io->ctx, data+total, step, &done, tm);
        total += done;
    }
    *sent = total;
    buf->sent += total;
    return err;
}

static int sendostream(lua_State *L, p_buffer buf, size_t *sent) {
    int err = IO_DONE;
	t_userdata *userdata = buf->userdata;
	const char *ptr = stream_ptr(userdata->ostream);
	size_t size = stream_size(userdata->ostream);
	err = sendraw(buf, ptr, size, sent);
	stream_remove(userdata->ostream, 0, *sent);
	return err;
}

int buffer_meth_setreader(lua_State *L, p_buffer buf) {
	t_userdata* userdata = buf->userdata;
	if (userdata->istream)
		stream_unref(L, userdata->istream);
	userdata->istream = stream_ref(L, 2);
	return 0;
}

int buffer_meth_setwriter(lua_State *L, p_buffer buf) {
	t_userdata* userdata = buf->userdata;
	if (userdata->ostream)
		stream_unref(L, userdata->ostream);
	userdata->ostream = stream_ref(L, 2);
	return 0;
}

int buffer_meth_getreader(lua_State *L, p_buffer buf) {
	t_userdata* userdata = buf->userdata;
	buffer_checkclosed(L, buf);
	stream_push(L, userdata->istream);
	return 1;
}

int buffer_meth_getwriter(lua_State *L, p_buffer buf) {
	t_userdata* userdata = buf->userdata;
	buffer_checkclosed(L, buf);
	stream_push(L, userdata->ostream);
	return 1;
}

/*-------------------------------------------------------------------------*\
* Reads a package
\*-------------------------------------------------------------------------*/
static int recvistream(lua_State *L, p_buffer buf, size_t *size) {
    int err = IO_DONE;
	t_userdata* userdata = buf->userdata;
	const char *data;
	
	if (stream_tell(userdata->istream) > 0)
		stream_remove(userdata->istream, 0, stream_tell(userdata->istream));
	err = buffer_get(buf, &data, size);
	stream_write(userdata->istream, data, *size);
	buffer_skip(buf, *size);
    return err;
}

/*-------------------------------------------------------------------------*\
* Reads a fixed number of bytes (buffered)
\*-------------------------------------------------------------------------*/
static int recvraw(p_buffer buf, size_t wanted, luaL_Buffer *b) {
    int err = IO_DONE;
    size_t total = 0;
    while (err == IO_DONE) {
        size_t count; const char *data;
        err = buffer_get(buf, &data, &count);
        count = MIN(count, wanted - total);
        luaL_addlstring(b, data, count);
        buffer_skip(buf, count);
        total += count;
        if (total >= wanted) break;
    }
    return err;
}

/*-------------------------------------------------------------------------*\
* Reads everything until the connection is closed (buffered)
\*-------------------------------------------------------------------------*/
static int recvall(p_buffer buf, luaL_Buffer *b) {
    int err = IO_DONE;
    size_t total = 0;
    while (err == IO_DONE) {
        const char *data; size_t count;
        err = buffer_get(buf, &data, &count);
        total += count;
        luaL_addlstring(b, data, count);
        buffer_skip(buf, count);
    }
    if (err == IO_CLOSED) {
        if (total > 0) return IO_DONE;
        else return IO_CLOSED;
    } else return err;
}

/*-------------------------------------------------------------------------*\
* Reads a line terminated by a CR LF pair or just by a LF. The CR and LF 
* are not returned by the function and are discarded from the buffer
\*-------------------------------------------------------------------------*/
static int recvline(p_buffer buf, luaL_Buffer *b) {
    int err = IO_DONE;
    while (err == IO_DONE) {
        size_t count, pos; const char *data;
        err = buffer_get(buf, &data, &count);
        pos = 0;
        while (pos < count && data[pos] != '\n') {
            /* we ignore all \r's */
            if (data[pos] != '\r') luaL_putchar(b, data[pos]);
            pos++;
        }
        if (pos < count) { /* found '\n' */
            buffer_skip(buf, pos+1); /* skip '\n' too */
            break; /* we are done */
        } else /* reached the end of the buffer */
            buffer_skip(buf, pos);
    }
    return err;
}

/*-------------------------------------------------------------------------*\
* Skips a given number of bytes from read buffer. No data is read from the
* transport layer
\*-------------------------------------------------------------------------*/
static void buffer_skip(p_buffer buf, size_t count) {
    buf->received += count;
    buf->first += count;
    if (buffer_isempty(buf)) 
        buf->first = buf->last = 0;
}

/*-------------------------------------------------------------------------*\
* Return any data available in buffer, or get more data from transport layer
* if buffer is empty
\*-------------------------------------------------------------------------*/
static int buffer_get(p_buffer buf, const char **data, size_t *count) {
    int err = IO_DONE;
    p_io io = buf->io;
    p_timeout tm = buf->tm;
    if (buffer_isempty(buf)) {
        size_t got;
        err = io->recv(io->ctx, buf->data, BUF_SIZE, &got, tm);
        buf->first = 0;
        buf->last = got;
    }
    *count = buf->last - buf->first;
    *data = buf->data + buf->first;
    return err;
}
