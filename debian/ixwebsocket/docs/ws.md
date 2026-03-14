## General

ws is a command line tool that should exercise most of the IXWebSocket code, and provide example code.

```
ws is a websocket tool
Usage: ws [OPTIONS] SUBCOMMAND

Options:
  -h,--help                   Print this help message and exit

Subcommands:
  send                        Send a file
  receive                     Receive a file
  transfer                    Broadcasting server
  connect                     Connect to a remote server
  chat                        Group chat
  echo_server                 Echo server
  broadcast_server            Broadcasting server
  ping                        Ping pong
  curl                        HTTP Client
  httpd                       HTTP server
```

## curl

The curl subcommand try to be compatible with the curl syntax, to fetch http pages.

Making a HEAD request with the -I parameter.

```
$ ws curl -I https://www.google.com/

Accept-Ranges: none
Alt-Svc: quic=":443"; ma=2592000; v="46,43",h3-Q048=":443"; ma=2592000,h3-Q046=":443"; ma=2592000,h3-Q043=":443"; ma=2592000
Cache-Control: private, max-age=0
Content-Type: text/html; charset=ISO-8859-1
Date: Tue, 08 Oct 2019 21:36:57 GMT
Expires: -1
P3P: CP="This is not a P3P policy! See g.co/p3phelp for more info."
Server: gws
Set-Cookie: NID=188=ASwfz8GrXQrHCLqAz-AndLOMLcz0rC9yecnf3h0yXZxRL3rTufTU_GDDwERp7qQL7LZ_EB8gCRyPXGERyOSAgaqgnrkoTmvWrwFemRLMaOZ896GrHobi5fV7VLklnSG2w48Gj8xMlwxfP7Z-bX-xR9UZxep1tHM6UmFQdD_GkBE; expires=Wed, 08-Apr-2020 21:36:57 GMT; path=/; domain=.google.com; HttpOnly
Transfer-Encoding: chunked
Vary: Accept-Encoding
X-Frame-Options: SAMEORIGIN
X-XSS-Protection: 0
Upload size: 143
Download size: 0
Status: 200
```

Making a POST request with the -F parameter.

```
$ ws curl -F foo=bar https://httpbin.org/post
foo: bar
Downloaded 438 bytes out of 438
Access-Control-Allow-Credentials: true
Access-Control-Allow-Origin: *
Connection: keep-alive
Content-Encoding:
Content-Length: 438
Content-Type: application/json
Date: Tue, 08 Oct 2019 21:47:54 GMT
Referrer-Policy: no-referrer-when-downgrade
Server: nginx
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
X-XSS-Protection: 1; mode=block
Upload size: 219
Download size: 438
Status: 200
payload: {
  "args": {},
  "data": "",
  "files": {},
  "form": {
    "foo": "bar"
  },
  "headers": {
    "Accept": "*/*",
    "Content-Length": "7",
    "Content-Type": "application/x-www-form-urlencoded",
    "Host": "httpbin.org",
    "User-Agent": "ixwebsocket/7.0.0 macos ssl/OpenSSL OpenSSL 1.0.2q  20 Nov 2018 zlib 1.2.11"
  },
  "json": null,
  "origin": "155.94.127.118, 155.94.127.118",
  "url": "https://httpbin.org/post"
}
```

Passing in a custom header with -H.

```
$ ws curl -F foo=bar -H 'my_custom_header: baz' https://httpbin.org/post
my_custom_header:  baz
foo: bar
Downloaded 470 bytes out of 470
Access-Control-Allow-Credentials: true
Access-Control-Allow-Origin: *
Connection: keep-alive
Content-Encoding:
Content-Length: 470
Content-Type: application/json
Date: Tue, 08 Oct 2019 21:50:25 GMT
Referrer-Policy: no-referrer-when-downgrade
Server: nginx
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
X-XSS-Protection: 1; mode=block
Upload size: 243
Download size: 470
Status: 200
payload: {
  "args": {},
  "data": "",
  "files": {},
  "form": {
    "foo": "bar"
  },
  "headers": {
    "Accept": "*/*",
    "Content-Length": "7",
    "Content-Type": "application/x-www-form-urlencoded",
    "Host": "httpbin.org",
    "My-Custom-Header": "baz",
    "User-Agent": "ixwebsocket/7.0.0 macos ssl/OpenSSL OpenSSL 1.0.2q  20 Nov 2018 zlib 1.2.11"
  },
  "json": null,
  "origin": "155.94.127.118, 155.94.127.118",
  "url": "https://httpbin.org/post"
}
```

## connect

The connect command connects to a websocket endpoint, and starts an interactive prompt. Line editing, such as using the direction keys to fetch the last thing you tried to type) is provided. That command is pretty useful to try to send random data to an endpoint and verify that the service handles it with grace (such as sending invalid json).

```
ws connect wss://echo.websocket.org
Type Ctrl-D to exit prompt...
Connecting to url: wss://echo.websocket.org
> ws_connect: connected
Uri: /
Handshake Headers:
Connection: Upgrade
Date: Tue, 08 Oct 2019 21:38:44 GMT
Sec-WebSocket-Accept: 2j6LBScZveqrMx1W/GJkCWvZo3M=
sec-websocket-extensions:
Server: Kaazing Gateway
Upgrade: websocket
Received ping
Received ping
Received ping
Hello world !
> Received 13 bytes
ws_connect: received message: Hello world !
> Hello world !
> Received 13 bytes
ws_connect: received message: Hello world !
```

```
ws connect 'ws://jeanserge.com/v2?appkey=_pubsub'
Type Ctrl-D to exit prompt...
Connecting to url: ws://jeanserge.com/v2?appkey=_pubsub
> ws_connect: connected
Uri: /v2?appkey=_pubsub
Handshake Headers:
Connection: Upgrade
Date: Tue, 08 Oct 2019 21:45:28 GMT
Sec-WebSocket-Accept: LYHmjh9Gsu/Yw7aumQqyPObOEV4=
Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=15; client_max_window_bits=15
Server: Python/3.7 websockets/8.0.2
Upgrade: websocket
bababababababab
> ws_connect: connection closed: code 1000 reason

ws_connect: connected
Uri: /v2?appkey=_pubsub
Handshake Headers:
Connection: Upgrade
Date: Tue, 08 Oct 2019 21:45:44 GMT
Sec-WebSocket-Accept: I1rqxdLgTU+opPi5/zKPBTuXdLw=
Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=15; client_max_window_bits=15
Server: Python/3.7 websockets/8.0.2
Upgrade: websocket
```

It is possible to pass custom HTTP header when doing the connection handshake,
the remote server might process them to implement a simple authorization
scheme.

```
src$ ws connect -H Authorization:supersecret ws://localhost:8008
Type Ctrl-D to exit prompt...
[2020-12-17 22:35:08.732] [info] Authorization: supersecret
Connecting to url: ws://localhost:8008
> [2020-12-17 22:35:08.736] [info] ws_connect: connected
[2020-12-17 22:35:08.736] [info] Uri: /
[2020-12-17 22:35:08.736] [info] Headers:
[2020-12-17 22:35:08.736] [info] Connection: Upgrade
[2020-12-17 22:35:08.736] [info] Sec-WebSocket-Accept: 2yaTFcdwn8KL6IzSMj2u6Le7KTg=
[2020-12-17 22:35:08.736] [info] Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=15; client_max_window_bits=15
[2020-12-17 22:35:08.736] [info] Server: ixwebsocket/11.0.4 macos ssl/SecureTransport zlib 1.2.11
[2020-12-17 22:35:08.736] [info] Upgrade: websocket
[2020-12-17 22:35:08.736] [info] Received 25 bytes
ws_connect: received message: Authorization suceeded!
[2020-12-17 22:35:08.736] [info] Received pong ixwebsocket::heartbeat::30s::0
hello
> [2020-12-17 22:35:25.157] [info] Received 7 bytes
ws_connect: received message: hello
```

If the wrong header is passed in, the server would close the connection with a custom close code (>4000, and <4999).

```
[2020-12-17 22:39:37.044] [info] Upgrade: websocket
ws_connect: connection closed: code 4001 reason Permission denied
```

## echo server

The ws echo server will respond what the client just sent him. If we use the
simple --http_authorization_header we can enforce that client need to pass a
special value in the Authorization header to connect.

```
$ ws echo_server --http_authorization_header supersecret
[2020-12-17 22:35:06.192] [info] Listening on 127.0.0.1:8008
[2020-12-17 22:35:08.735] [info] New connection
[2020-12-17 22:35:08.735] [info] remote ip: 127.0.0.1
[2020-12-17 22:35:08.735] [info] id: 0
[2020-12-17 22:35:08.735] [info] Uri: /
[2020-12-17 22:35:08.735] [info] Headers:
[2020-12-17 22:35:08.735] [info] Authorization: supersecret
[2020-12-17 22:35:08.735] [info] Connection: Upgrade
[2020-12-17 22:35:08.735] [info] Host: localhost:8008
[2020-12-17 22:35:08.735] [info] Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=15; client_max_window_bits=15
[2020-12-17 22:35:08.735] [info] Sec-WebSocket-Key: eFF2Gf25dC7eC15Ab1135G==
[2020-12-17 22:35:08.735] [info] Sec-WebSocket-Version: 13
[2020-12-17 22:35:08.735] [info] Upgrade: websocket
[2020-12-17 22:35:08.735] [info] User-Agent: ixwebsocket/11.0.4 macos ssl/SecureTransport zlib 1.2.11
[2020-12-17 22:35:25.157] [info] Received 7 bytes
```

## Websocket proxy

```
ws proxy_server --remote_host ws://127.0.0.1:9000 -v
Listening on 127.0.0.1:8008
```

If you connect to ws://127.0.0.1:8008, the proxy will connect to ws://127.0.0.1:9000 and pass all traffic to this server.

You can also use a more complex setup if you want to redirect to different websocket servers based on the hostname your client is trying to connect to. If you have multiple CNAME aliases that point to the same server.

A JSON config file is used to express that mapping ; here connecting to echo.jeanserge.com will proxy the client to ws://localhost:8008 on the local machine (which actually runs ws echo_server), while connecting to bavarde.jeanserge.com will proxy the client to ws://localhost:5678 where a cobra python server is running. As a side note you will need a wildcard SSL certificate if you want to have SSL enabled on that machine.

```
echo.jeanserge.com=ws://localhost:8008
bavarde.jeanserge.com=ws://localhost:5678
```
The --config_path option is required to instruct ws proxy_server to read that file.

```
ws proxy_server --config_path proxyConfig.json --port 8765
```

## File transfer

```
# Start transfer server, which is just a broadcast server at this point
ws transfer # running on port 8080.

# Start receiver first
ws receive ws://localhost:8080

# Then send a file. File will be received and written to disk by the receiver process
ws send ws://localhost:8080 /file/to/path
```

## HTTP Client

```
$ ws curl --help
HTTP Client
Usage: ws curl [OPTIONS] url

Positionals:
  url TEXT REQUIRED           Connection url

Options:
  -h,--help                   Print this help message and exit
  -d TEXT                     Form data
  -F TEXT                     Form data
  -H TEXT                     Header
  --output TEXT               Output file
  -I                          Send a HEAD request
  -L                          Follow redirects
  --max-redirects INT         Max Redirects
  -v                          Verbose
  -O                          Save output to disk
  --compress                  Enable gzip compression
  --connect-timeout INT       Connection timeout
  --transfer-timeout INT      Transfer timeout
```
