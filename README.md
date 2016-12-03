Name
====

nginx_stream_upstream_check_module - support stream upstream health check with Nginx, can work with nginx-stream-upsync-moudle. The module is mainly based on [nginx_tcp_proxy_module](https://github.com/yaoweibin/nginx_tcp_proxy_module)

Table of Contents
=================

* [Name](#name)
* [Status](#status)
* [Synopsis](#synopsis)
* [Description](#description)
* [Directives](#directives)
    * [check](#check)
        * [interval](#interval)
        * [rise](#rise)
        * [fall](#fall)
        * [timeout](#timeout)
        * [type](#type)
    * [check_http_send](#check_http_send)
    * [check_http_expect_alive](#check_http_expect_alive)
    * [check_shm_size](#check_shm_size)
    * [stream_check_status](#stream_check_status)
* [TODO](#todo)
* [Compatibility](#compatibility)
* [Installation](#installation)
* [Author](#author)
* [Copyright and License](#copyright-and-license)

Status
======

This module is still active development, can be used production environment.

Synopsis
====

```
stream {

    upstream cluster {

        # simple round-robin
        server 192.168.0.1:80;
        server 192.168.0.2:80;

        check interval=3000 rise=2 fall=5 timeout=1000 type=http;
        check_http_send "GET /proxy_test HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 8081;

        proxy_pass stream://cluster;
    }

    server {
        listen 9091;

        stream_check_status;
    }

}
```

Description
====

Add the support of health check with the stream upstream servers.

Directives
====

check
--------
```
syntax: check interval=milliseconds [fall=count] [rise=count] [timeout=milliseconds] [type=tcp|http|ssl_hello|mysql|pop3|imap]
```
default: none, if parameters omitted, default parameters are interval=30000 fall=5 rise=2 timeout=1000 type=tcp

context: upstream

description: Add the health check for the upstream servers.

The parameters' meanings are:

* interval: the check request's interval time.

* fall(fall_count): After fall_count check failures, the server is marked down.

* rise(rise_count): After rise_count check success, the server is marked up.

* timeout: the check request's timeout.

* type: the check protocol type:

        1.  *tcp* is a simple tcp socket connect and peek one byte.

        2.  *ssl_hello* sends a client ssl hello packet and receives the
            server ssl hello packet.

        3.  *http* sends a http request packet, receives and parses the http
            response to diagnose if the upstream server is alive.

        4.  *mysql* connects to the mysql server, receives the greeting
            response to diagnose if the upstream server is alive.

        5.  *ajp* sends a AJP Cping packet, receives and parses the AJP
            Cpong response to diagnose if the upstream server is alive.

check_http_send
--------
```
syntax: check_http_send http_packet
```
default: "GET / HTTP/1.0\r\n\r\n"

context: upstream

description: If you set the check type is stream, then the check function will sends this http packet to check the upstream server.

check_stream_expect_alive
--------
```
syntax: check_http_expect_alive [ http_2xx | http_3xx | http_4xx | stream_5xx ]
```
default: http_2xx | http_3xx

context: upstream

description: These status codes indicate the upstream server's stream response is ok, the backend is alive.

check_shm_size
--------
```
syntax: check_shm_size size
```
default: 1M

context: stream

description: Default size is one megabytes. If you check thousands of servers, the shared memory for health check may be not enough, you can
             enlarge it with this directive.

stream_check_status
--------
```
syntax: stream_check_status
```
default: none

context: server

description: Display the health checking servers' status by HTTP. This directive should be set in the server block.

[Back to TOC](#table-of-contents)

TODO
====

* test for ssl_hello and mysql.

[Back to TOC](#table-of-contents)

Compatibility
=============

The module was developed based on nginx-1.10.1.

Compatible with Nginx-1.9.0+.

[Back to TOC](#table-of-contents)

Installation
====
This module can be used independently, can be download[Github](https://github.com/xiaokai-wang/nginx_stream_upstream_check_module.git).

Grab the nginx source code from [nginx.org](http://nginx.org/), for example, the version 1.10.1 (see nginx compatibility), and then build the source with this module:

```bash
wget 'http://nginx.org/download/nginx-1.10.1.tar.gz'
tar -xzvf nginx-1.10.1.tar.gz
cd nginx-1.10.1/
```

```bash
./configure --with-stream --add-module=/path/to/nginx_stream_upstream_check_module
make
make install
```

[Back to TOC](#table-of-contents)

Author
====

Xiaokai Wang(王晓开)  xiaokai.wang@live.com

Weibin Yao

Copyright & License
====

This README template copy from agentzh (<stream://github.com/agentzh>).

The health check part is based on the design of Weibin Yao's tcp module [nginx_tcp_proxy_module](https://github.com/yaoweibin/nginx_tcp_proxy_module);

This module is licensed under the BSD license.

Copyright (C) 2016 by Xiaokai Wang <xiaokai.wang@live.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
