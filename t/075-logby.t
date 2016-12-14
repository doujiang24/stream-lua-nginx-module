# vim:set ft= ts=4 sw=4 et fdm=marker:

use Test::Nginx::Socket::Lua::Stream;
#worker_connections(1014);
#master_on();
#workers(2);
log_level('debug');

repeat_each(2);

plan tests => repeat_each() * (blocks() * 3 + 9);

#no_diff();
#no_long_string();
run_tests();

__DATA__

=== TEST 1: log_by_lua
--- stream_server_config
    echo hello;
    log_by_lua_block {
        ngx.log(ngx.ERR, "Hello from log_by_lua.")
    }
--- stream_response
hello
--- error_log
Hello from log_by_lua.



=== TEST 2: log_by_lua_file
--- stream_server_config
    echo hello;
    log_by_lua_file html/a.lua;
--- user_files
>>> a.lua
ngx.log(ngx.ERR, "Hello from log_by_lua.")
--- stream_response
hello
--- error_log
Hello from log_by_lua.



=== TEST 3: log_by_lua_file & content_by_lua
--- stream_server_config
    content_by_lua_block {
        ngx.ctx.counter = 3
        ngx.ctx.counter = ngx.ctx.counter + 1
        ngx.say(ngx.ctx.counter)
    }
    log_by_lua_file html/a.lua;
--- user_files
>>> a.lua
ngx.log(ngx.ERR, "Hello from log_by_lua: ", ngx.ctx.counter * 2)
--- stream_response
4
--- error_log
Hello from log_by_lua: 8



=== TEST 4: ngx.ctx available in log_by_lua (already defined)
--- stream_server_config
    content_by_lua_block {
        ngx.ctx.counter = 3
        ngx.say(ngx.ctx.counter)
    }
    log_by_lua_block {
        ngx.log(ngx.ERR, "ngx.ctx.counter: ", ngx.ctx.counter)
    }
--- stream_response
3
--- error_log
ngx.ctx.counter: 3
lua release ngx.ctx



=== TEST 5: ngx.ctx available in log_by_lua (not defined yet)
--- stream_server_config
    echo hello;
    log_by_lua_block {
        ngx.log(ngx.ERR, "ngx.ctx.counter: ", ngx.ctx.counter)
        ngx.ctx.counter = "hello world"
    }
--- stream_response
hello
--- error_log
ngx.ctx.counter: nil
lua release ngx.ctx



=== TEST 6: log_by_lua + shared dict
--- stream_config
    lua_shared_dict foo 100k;
--- stream_server_config
    echo hello;
    log_by_lua_block {
        local foo = ngx.shared.foo
        local key = "foo"
        local newval, err = foo:incr(key, 1)
        if not newval then
            if err == "not found" then
                foo:add(key, 0)
                newval, err = foo:incr(key, 1)
                if not newval then
                    ngx.log(ngx.ERR, "failed to incr ", key, ": ", err)
                    return
                end
            else
                ngx.log(ngx.ERR, "failed to incr ", key, ": ", err)
                return
            end
        end
        print(key, ": ", foo:get(key))
    }
--- stream_response
hello
--- error_log eval
qr{foo: [12]}
--- no_error_log
[error]



=== TEST 7: ngx.ctx used in different servers and different ctx (1)
--- stream_server_config
    echo hello;
    log_by_lua_block {
            ngx.log(ngx.ERR, "ngx.ctx.counter: ", ngx.ctx.counter)
    }

--- stream_server_config2
    content_by_lua_block {
        ngx.ctx.counter = 32
        ngx.say("hello")
    }
--- stream_response
hello
hello
--- error_log
ngx.ctx.counter: nil
lua release ngx.ctx



=== TEST 8: ngx.ctx used in different locations and different ctx (2)
--- stream_server_config
    echo hello;
    log_by_lua_block {
        ngx.log(ngx.ERR, "ngx.ctx.counter: ", ngx.ctx.counter)
    }

    content_by_lua_block {
        ngx.ctx.counter = 32
        ngx.say(ngx.ctx.counter)
    }
--- stream_response
32
--- error_log
lua release ngx.ctx



=== TEST 9: lua error (string)
--- stream_server_config
    log_by_lua_block { error("Bad") }
    echo ok;
--- stream_response
ok
--- error_log eval
qr/failed to run log_by_lua\*: log_by_lua_block\(nginx\.conf:\d+\):1: Bad/



=== TEST 10: lua error (nil)
--- stream_server_config
    log_by_lua_block { error(nil) }
    echo ok;
--- stream_response
ok
--- error_log
failed to run log_by_lua*: unknown reason



=== TEST 11: globals get cleared for every single request
--- stream_server_config
    echo ok;
    log_by_lua_block {
        if not foo then
            foo = 1
        else
            foo = foo + 1
        end
        ngx.log(ngx.WARN, "foo = ", foo)
    }
--- stream_response
ok
--- error_log
foo = 1



=== TEST 12: no ngx.print
--- stream_server_config
    log_by_lua_block { ngx.print(32) return 1 }
    echo ok;
--- stream_response
ok
--- error_log
API disabled in the context of log_by_lua*



=== TEST 13: no ngx.say
--- stream_server_config
    log_by_lua_block { ngx.say(32) return 1 }
    echo ok;
--- stream_response
ok
--- error_log
API disabled in the context of log_by_lua*



=== TEST 14: no ngx.flush
--- stream_server_config
    log_by_lua_block { ngx.flush() }
    echo ok;
--- stream_response
ok
--- error_log
API disabled in the context of log_by_lua*



=== TEST 15: no ngx.eof
--- stream_server_config
    log_by_lua_block { ngx.eof() }
    echo ok;
--- stream_response
ok
--- error_log
API disabled in the context of log_by_lua*



=== TEST 16: no ngx.req.socket()
--- stream_server_config
    log_by_lua_block { return ngx.req.socket() }
    echo ok;
--- stream_response
ok
--- error_log
API disabled in the context of log_by_lua*



=== TEST 17: no ngx.socket.tcp()
--- stream_server_config
    log_by_lua_block { return ngx.socket.tcp() }
    echo ok;
--- stream_response
ok
--- error_log
API disabled in the context of log_by_lua*



=== TEST 18: no ngx.socket.connect()
--- stream_server_config
    log_by_lua_block { return ngx.socket.connect("127.0.0.1", 80) }
    echo ok;
--- stream_response
ok
--- error_log
API disabled in the context of log_by_lua*



=== TEST 19: backtrace
--- stream_server_config
    echo ok;
    log_by_lua_block {
        function foo()
            bar()
        end

        function bar()
            error("something bad happened")
        end

        foo()
    }
--- stream_response
ok
--- error_log
something bad happened
stack traceback:
in function 'error'
in function 'bar'
in function 'foo'



=== TEST 20: Lua file does not exist
--- stream_server_config
        echo ok;
        log_by_lua_file html/test2.lua;
--- user_files
>>> test.lua
v = ngx.var["request_uri"]
ngx.print("request_uri: ", v, "\n")
--- stream_response
ok
--- error_log eval
qr/failed to load external Lua file ".*?test2\.lua": cannot open .*? No such file or directory/



=== TEST 21: log_by_lua runs before access logging (github issue #254)
--- stream_config
    log_format proxy '$remote_addr [$time_local] ';
--- stream_server_config
    echo ok;
    access_log logs/foo.log proxy;
    log_by_lua_block { print("hello") }
--- stap
F(ngx_stream_log_handler) {
    println("log handler")
}
F(ngx_stream_lua_log_handler) {
    println("lua log handler")
}
--- stap_out
lua log handler
log handler

--- stream_response
ok
--- no_error_log
[error]
