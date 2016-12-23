
/*
 * Copyright (C) Yichun Zhang (agentzh)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_stream_lua_variable.h"
#include "ngx_stream_lua_util.h"


static int ngx_stream_lua_var_get(lua_State *L);
static int ngx_stream_lua_var_set(lua_State *L);


void
ngx_stream_lua_inject_variable_api(lua_State *L)
{
    /* {{{ register reference maps */
    lua_newtable(L);    /* ngx.var */

    lua_createtable(L, 0, 2 /* nrec */); /* metatable for .var */
    lua_pushcfunction(L, ngx_stream_lua_var_get);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ngx_stream_lua_var_set);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);

    lua_setfield(L, -2, "var");
}


/* Get pseudo NGINX variables content
 *
 * @retval Always return a string or nil on Lua stack. Return nil when failed
 * to get content, and actual content string when found the specified variable.
 */
static int
ngx_stream_lua_var_get(lua_State *L)
{
    ngx_stream_session_t        *s;
    u_char                      *p, *lowcase;
    size_t                       len;
    ngx_uint_t                   hash;
    ngx_str_t                    name;
    ngx_stream_variable_value_t *vv;

    s = ngx_stream_lua_get_session(L);
    if (s == NULL) {
        return luaL_error(L, "no session found");
    }

    ngx_stream_lua_check_fake_session(L, s);

    if (lua_type(L, -1) != LUA_TSTRING) {
        return luaL_error(L, "bad variable name");
    }

    p = (u_char *) lua_tolstring(L, -1, &len);

    lowcase = lua_newuserdata(L, len);

    hash = ngx_hash_strlow(lowcase, p, len);

    name.len = len;
    name.data = lowcase;

    vv = ngx_stream_get_variable(s, &name, hash);

    if (vv == NULL || vv->not_found) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *) vv->data, (size_t) vv->len);
    return 1;
}


/**
 * Can not set pseudo NGINX variables content
 * */
static int
ngx_stream_lua_var_set(lua_State *L)
{
    return luaL_error(L, "can not set variable");
}
