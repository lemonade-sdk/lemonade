# Changelog

All changes to this project will be documented in this file.

## [11.4.5] - 2024-06-05

New changes are documented in the Release page in the GitHub repository.

## [11.4.4] - 2023-06-05

## [11.4.3] - 2022-05-13

Set shorter thread names
BoringSSL fix with SNI
Websocket computed header is valid Base64

## [11.4.1] - 2022-04-23

vckpg + cmake fix, to handle zlib as a dependency better

## [11.4.0] - 2022-01-05

(Windows) Use wsa select event, which should lead to a much better behavior on Windows in general, and also when sending large payloads (#342)
Fix "HTTP/1.1 400 Illegal character CNTL=0xf" caused by serverMaxWindowBits/clientMaxWindowBits being uint8_t (signed char). (#341)
Export symbols into .def files
Export symbols into .def files  on MSVC (#339)
Include <cerrno> to provide standard error constants (#338)
Improved compatibility - fix mingw crossbuild (#337)
Allow to cancel asynchronous HTTP requests (#332)
Fix errors in example code. (#336)

## [11.3.2] - 2021-11-24

(server) Add getters for basic Servers properties (like port, host, etc...) (#327) + fix one compiler warning

## [11.3.1] - 2021-10-22

(library/cmake) Compatible with MbedTLS 3 + fix a bug on Windows where the incorrect remote port is computed (#320)

## [11.3.0] - 2021-09-20

(library/cmake) Only find OpenSSL, MbedTLS, zlib if they have not already been found, make CMake install optional (#317) + Use GNUInstallDirs in cmake (#318)

## [11.2.10] - 2021-07-27

(ws) bump CLI command line parsing library from 1.8 to 2.0

## [11.2.9] - 2021-06-08

(ws) ws connect has a -g option to gzip decompress messages for API such as the websocket Huobi Global.

## [11.2.8] - 2021-06-03

(websocket client + server) WebSocketMessage class tweak to fix unsafe patterns

## [11.2.7] - 2021-05-27

(websocket server) Handle and accept firefox browser special upgrade value (keep-alive, Upgrade)

## [11.2.6] - 2021-05-18

(Windows) move EINVAL (re)definition from IXSocket.h to IXNetSystem.h (fix #289)

## [11.2.5] - 2021-04-04

(http client) DEL is not an HTTP method name, but DELETE is

## [11.2.4] - 2021-03-25

(cmake) install IXUniquePtr.h

## [11.2.3] - 2021-03-24

(ssl + windows) missing include for CertOpenStore function

## [11.2.2] - 2021-03-23

(ixwebsocket) version bump

## [11.2.1] - 2021-03-23

(ixwebsocket) version bump

## [11.2.0] - 2021-03-23

(ixwebsocket) correct mingw support (gcc on windows)

## [11.1.4] - 2021-03-23

(ixwebsocket) add getMinWaitBetweenReconnectionRetries

## [11.1.3] - 2021-03-23

(ixwebsocket) New option to set the min wait between reconnection attempts. Still default to 1ms. (setMinWaitBetweenReconnectionRetries).

## [11.1.2] - 2021-03-22

(ws) initialize maxWaitBetweenReconnectionRetries to a non zero value ; a zero value was causing spurious reconnections attempts

## [11.1.1] - 2021-03-20

(cmake) Library can be built as a static or a dynamic library, controlled with BUILD_SHARED_LIBS. Default to static library

## [11.1.0] - 2021-03-16

(ixwebsocket) Use LEAN_AND_MEAN Windows define to help with undefined link error when building a DLL. Support websocket server disablePerMessageDeflate option correctly.

## [11.0.9] - 2021-03-07

(ixwebsocket) Expose setHandshakeTimeout method

## [11.0.8] - 2020-12-25

(ws) trim ws dependencies no more ixcrypto and ixcore deps

## [11.0.7] - 2020-12-25

(ws) trim ws dependencies, only depends on ixcrypto and ixcore

## [11.0.6] - 2020-12-22

(build) rename makefile to makefile.dev to ease cmake BuildExternal (fix #261)

## [11.0.5] - 2020-12-17

(ws) Implement simple header based websocket authorization technique to reject
client which do not supply a certain header ("Authorization") with a special
value (see doc).

## [11.0.4] - 2020-11-16

(ixwebsocket) Handle EINTR return code in ix::poll and IXSelectInterrupt

## [11.0.3] - 2020-11-16

(ixwebsocket) Fix #252 / regression in 11.0.2 with string comparisons

## [11.0.2] - 2020-11-15

(ixwebsocket) use a C++11 compatible make_unique shim

## [11.0.1] - 2020-11-11

(socket) replace a std::vector with an std::array used as a tmp buffer in Socket::readBytes

## [11.0.0] - 2020-11-11

(openssl security fix) in the client to server connection, peer verification is not done in all cases. See https://github.com/machinezone/IXWebSocket/pull/250

## [10.5.7] - 2020-11-07

(docker) build docker container with zlib disabled

## [10.5.6] - 2020-11-07

(cmake) DEFLATE -> Deflate in CMake to stop warnings about casing

## [10.5.5] - 2020-11-07

(ws autoroute) Display result in compliant way (AUTOROUTE IXWebSocket :: N ms) so that result can be parsed easily

## [10.5.4] - 2020-10-30

(ws gunzip + IXGZipCodec) Can decompress gziped data with libdeflate. ws gunzip computed output filename was incorrect (was the extension aka gz) instead of the file without the extension. Also check whether the output file is writeable.

## [10.5.3] - 2020-10-19

(http code) With zlib disabled, some code should not be reached

## [10.5.2] - 2020-10-12

(ws curl) Add support for --data-binary option, to set the request body. When present the request will be sent with the POST verb

## [10.5.1] - 2020-10-09

(http client + server + ws) Add support for compressing http client requests with gzip. --compress_request argument is used in ws to enable this. The Content-Encoding is set to gzip, and decoded on the server side if present.

## [10.5.0] - 2020-09-30

(http client + server + ws) Add support for uploading files with ws -F foo=@filename, new -D http server option to debug incoming client requests, internal api changed for http POST, PUT and PATCH to supply an HttpFormDataParameters

## [10.4.9] - 2020-09-30

(http server + utility code) Add support for doing gzip compression with libdeflate library, if available

## [10.4.8] - 2020-09-30

(cmake) Stop using FetchContent cmake module to retrieve jsoncpp third party dependency

## [10.4.7] - 2020-09-28

(ws) add gzip and gunzip ws sub commands

## [10.4.6] - 2020-09-26

(cmake) use FetchContent cmake module to retrieve jsoncpp third party dependency

## [10.4.5] - 2020-09-26

(cmake) use FetchContent cmake module to retrieve spdlog third party dependency

## [10.4.4] - 2020-09-22

(cobra connection) retrieve cobra server connection id from the cobra handshake message and display it in ws clients, metrics publisher and bots

## [10.4.3] - 2020-09-22

(cobra 2 cobra) specify as an HTTP header which channel we will republish to

## [10.4.2] - 2020-09-18

(cobra bots) change an error log to a warning log when reconnecting because no messages were received for a minute

## [10.4.1] - 2020-09-18

(cobra connection and bots) set an HTTP header when connecting to help with debugging bots

## [10.4.0] - 2020-09-12

(http server) read body request when the Content-Length is specified + set timeout to read the request to 30 seconds max by default, and make it configurable as a constructor parameter

## [10.3.5] - 2020-09-09

(ws) autoroute command exit on its own once all messages have been received

## [10.3.4] - 2020-09-04

(docker) ws docker file installs strace

## [10.3.3] - 2020-09-02

(ws) echo_client command renamed to autoroute. Command exit once the server close the connection. push_server commands exit once N messages have been sent.

## [10.3.2] - 2020-08-31

(ws + cobra bots) add a cobra_to_cobra ws subcommand to subscribe to a channel and republish received events to a different channel

## [10.3.1] - 2020-08-28

(socket servers) merge the ConnectionInfo class with the ConnectionState one, which simplify all the server apis

## [10.3.0] - 2020-08-26

(ws) set the main thread name, to help with debugging in XCode, gdb, lldb etc...

## [10.2.9] - 2020-08-19

(ws) cobra to python bot / take a module python name as argument foo.bar.baz instead of a path foo/bar/baz.py

## [10.2.8] - 2020-08-19

(ws) on Linux with mbedtls, when the system ca certs are specified (the default) pick up sensible OS supplied paths (tested with CentOS and Alpine)

## [10.2.7] - 2020-08-18

(ws push_server) on the server side, stop sending and close the connection when the remote end has disconnected

## [10.2.6] - 2020-08-17

(ixwebsocket) replace std::unique_ptr<unsigned char[]> with std::array for some fixed arrays (which are in C++11)

## [10.2.5] - 2020-08-15

(ws) merge all ws_*.cpp files into a single one to speedup compilation

## [10.2.4] - 2020-08-15

(socket server) in the loop accepting connections, call select without a timeout on unix to avoid busy looping, and only wake up when a new connection happens

## [10.2.3] - 2020-08-15

(socket server) instead of busy looping with a sleep, only wake up the GC thread when a new thread will have to be joined, (we know that thanks to the ConnectionState OnSetTerminated callback

## [10.2.2] - 2020-08-15

(socket server) add a callback to the ConnectionState to be invoked when the connection is terminated. This will be used by the SocketServer in the future to know on time that the associated connection thread can be terminated.

## [10.2.1] - 2020-08-15

(socket server) do not create a select interrupt object everytime when polling for notifications while waiting for new connections, instead use a persistent one which is a member variable

## [10.2.0] - 2020-08-14

(ixwebsocket client) handle HTTP redirects

## [10.2.0] - 2020-08-13

(ws) upgrade to latest version of nlohmann json (3.9.1 from 3.2.0)

## [10.1.9] - 2020-08-13

(websocket proxy server) add ability to map different hosts to different websocket servers, using a json config file

## [10.1.8] - 2020-08-12

(ws) on macOS, with OpenSSL or MbedTLS, use /etc/ssl/cert.pem as the system certs

## [10.1.7] - 2020-08-11

(ws) -q option imply info log level, not warning log level

## [10.1.6] - 2020-08-06

(websocket server) Handle programmer error when the server callback is not registered properly (fix #227)

## [10.1.5] - 2020-08-02

(ws) Add a new ws sub-command, push_server. This command runs a server which sends many messages in a loop to a websocket client. We can receive above 200,000 messages per second (cf #235).

## [10.1.4] - 2020-08-02

(ws) Add a new ws sub-command, echo_client. This command sends a message to an echo server, and send back to a server whatever message it does receive. When connecting to a local ws echo_server, on my MacBook Pro 2015 I can send/receive around 30,000 messages per second. (cf #235)

## [10.1.3] - 2020-08-02

(ws) ws echo_server. Add a -q option to only enable warning and error log levels. This is useful for bench-marking so that we do not print a lot of things on the console. (cf #235)

## [10.1.2] - 2020-07-31

(build) make using zlib optional, with the caveat that some http and websocket features are not available when zlib is absent

## [10.1.1] - 2020-07-29

(websocket client) onProgressCallback not called for short messages on a websocket (fix #233)

## [10.1.0] - 2020-07-29

(websocket client) heartbeat is not sent at the requested frequency (fix #232)

## [10.0.3] - 2020-07-28

compiler warning fixes

## [10.0.2] - 2020-07-28

(ixcobra) CobraConnection: unsubscribe from all subscriptions when disconnecting

## [10.0.1] - 2020-07-27

(socket utility) move ix::getFreePort to ixwebsocket library

## [10.0.0] - 2020-07-25

(ixwebsocket server) change legacy api with 2 nested callbacks, so that the first api takes a weak_ptr<WebSocket> as its first argument

## [9.10.7] - 2020-07-25

(ixwebsocket) add WebSocketProxyServer, from ws. Still need to make the interface better.

## [9.10.6] - 2020-07-24

(ws) port broadcast_server sub-command to the new server API

## [9.10.5] - 2020-07-24

(unittest) port most unittests to the new server API

## [9.10.3] - 2020-07-24

(ws) port ws transfer to the new server API

## [9.10.2] - 2020-07-24

(websocket client) reset WebSocketTransport onClose callback in the WebSocket destructor

## [9.10.1] - 2020-07-24

(websocket server) reset client websocket callback when the connection is closed

## [9.10.0] - 2020-07-23

(websocket server) add a new simpler API to handle client connections / that API does not trigger a memory leak while the previous one did

## [9.9.3] - 2020-07-17

(build) merge platform specific files which were used to have different implementations for setting a thread name into a single file, to make it easier to include every source files and build the ixwebsocket library (fix #226)

## [9.9.2] - 2020-07-10

(socket server) bump default max connection count from 32 to 128

## [9.9.1] - 2020-07-10

(snake) implement super simple stream sql expression support in snake server

## [9.9.0] - 2020-07-08

(socket+websocket+http+redis+snake servers) expose the remote ip and remote port when a new connection is made

## [9.8.6] - 2020-07-06

(cmake) change the way zlib and openssl are searched

## [9.8.5] - 2020-07-06

(cobra python bots) remove the test which stop the bot when events do not follow cobra metrics system schema with an id and a device entry

## [9.8.4] - 2020-06-26

(cobra bots) remove bots which is not required now that we can use Python extensions

## [9.8.3] - 2020-06-25

(cmake) new python code is optional and enabled at cmake time with -DUSE_PYTHON=1

## [9.8.2] - 2020-06-24

(cobra bots) new cobra metrics bot to send data to statsd using Python for processing the message

## [9.8.1] - 2020-06-19

(cobra metrics to statsd bot) fps slow frame info : do not include os name

## [9.8.0] - 2020-06-19

(cobra metrics to statsd bot) send info about memory warnings

## [9.7.9] - 2020-06-18

(http client) fix deadlock when following redirects

## [9.7.8] - 2020-06-18

(cobra metrics to statsd bot) send info about net requests

## [9.7.7] - 2020-06-17

(cobra client and bots) add batch_size subscription option for retrieving multiple messages at once

## [9.7.6] - 2020-06-15

(websocket) WebSocketServer is not a final class, so that users can extend it (fix #215)

## [9.7.5] - 2020-06-15

(cobra bots) minor aesthetic change, in how we display http headers with a : then space as key value separator instead of :: with no space

## [9.7.4] - 2020-06-11

(cobra metrics to statsd bot) change from a statsd type of gauge to a timing one

## [9.7.3] - 2020-06-11

(redis cobra bots) capture most used devices in a zset

## [9.7.2] - 2020-06-11

(ws) add bare bone redis-cli like sub-command, with command line editing powered by libnoise

## [9.7.1] - 2020-06-11

(redis cobra bots) ws cobra metrics to redis / hostname invalid parsing

## [9.7.0] - 2020-06-11

(redis cobra bots) xadd with maxlen + fix bug in xadd client implementation and ws cobra metrics to redis command argument parsing

## [9.6.9] - 2020-06-10

(redis cobra bots) update the cobra to redis bot to use the bot framework, and change it to report fps metrics into redis streams.

## [9.6.6] - 2020-06-04

(statsd cobra bots) statsd improvement: prefix does not need a dot as a suffix, message size can be larger than 256 bytes, error handling was invalid, use core logger for logging instead of std::cerr

## [9.6.5] - 2020-05-29

(http server) support gzip compression

## [9.6.4] - 2020-05-20

(compiler fix) support clang 5 and earlier (contributed by @LunarWatcher)

## [9.6.3] - 2020-05-18

(cmake) revert CMake changes to fix #203 and be able to use an external OpenSSL

## [9.6.2] - 2020-05-17

(cmake) make install cmake files optional to not conflict with vcpkg

## [9.6.1] - 2020-05-17

(windows + tls) mbedtls is the default windows tls backend + add ability to load system certificates with mbdetls on windows

## [9.6.0] - 2020-05-12

(ixbots) add options to limit how many messages per minute should be processed

## [9.5.9] - 2020-05-12

(ixbots) add new class to configure a bot to simplify passing options around

## [9.5.8] - 2020-05-08

(openssl tls) (openssl < 1.1) logic inversion - crypto locking callback are not registered properly

## [9.5.7] - 2020-05-08

(cmake) default TLS back to mbedtls on Windows Universal Platform

## [9.5.6] - 2020-05-06

(cobra bots) add a --heartbeat_timeout option to specify when the bot should terminate because no events are received

## [9.5.5] - 2020-05-06

(openssl tls) when OpenSSL is older than 1.1, register the crypto locking callback to be thread safe. Should fix lots of CI failures

## [9.5.4] - 2020-05-04

(cobra bots) do not use a queue to store messages pending processing, let the bot handle queuing

## [9.5.3] - 2020-04-29

(http client) better current request cancellation support when the HttpClient destructor is invoked (see #189)

## [9.5.2] - 2020-04-27

(cmake) fix cmake broken tls option parsing

## [9.5.1] - 2020-04-27

(http client) Set default values for most HttpRequestArgs struct members (fix #185)

## [9.5.0] - 2020-04-25

(ssl) Default to OpenSSL on Windows, since it can load the system certificates by default

## [9.4.1] - 2020-04-25

(header) Add a space between header name and header value since most http parsers expects it, although it it not required. Cf #184 and #155

## [9.4.0] - 2020-04-24

(ssl) Add support for supplying SSL CA from memory, for OpenSSL and MbedTLS backends

## [9.3.3] - 2020-04-17

(ixbots) display sent/receive message, per seconds as accumulated

## [9.3.2] - 2020-04-17

(ws) add a --logfile option to configure all logs to go to a file

## [9.3.1] - 2020-04-16

(cobra bots) add a utility class to factor out the common bots features (heartbeat) and move all bots to used it + convert cobra_subscribe to be a bot and add a unittest for it

## [9.3.0] - 2020-04-15

(websocket) add a positive number to the heartbeat message sent, incremented each time the heartbeat is sent

## [9.2.9] - 2020-04-15

(ixcobra) change cobra event callback to use a struct instead of several objects, which is more flexible/extensible

## [9.2.8] - 2020-04-15

(ixcobra) make CobraConnection_EventType an enum class (CobraEventType)

## [9.2.7] - 2020-04-14

(ixsentry) add a library method to upload a payload directly to sentry

## [9.2.6] - 2020-04-14

(ixcobra) snake server / handle invalid incoming json messages + cobra subscriber in fluentd mode insert a created_at timestamp entry

## [9.2.5] - 2020-04-13

(websocket) WebSocketMessagePtr is a unique_ptr instead of a shared_ptr

## [9.2.4] - 2020-04-13

(websocket) use persistent member variable as temp variables to encode/decode zlib messages in order to reduce transient allocations

## [9.2.3] - 2020-04-13

(ws) add a --runtime option to ws cobra_subscribe to optionally limit how much time it will run

## [9.2.2] - 2020-04-04

(third_party deps) fix #177, update bundled spdlog to 1.6.0

## [9.2.1] - 2020-04-04

(windows) when using OpenSSL, the system store is used to populate the cacert. No need to ship a cacert.pem file with your app.

## [9.2.0] - 2020-04-04

(windows) ci: windows build with TLS (mbedtls) + verify that we can be build with OpenSSL

## [9.1.9] - 2020-03-30

(cobra to statsd bot) add ability to extract a numerical value and send a timer event to statsd, with the --timer option

## [9.1.8] - 2020-03-29

(cobra to statsd bot) bot init was missing + capture socket error

## [9.1.7] - 2020-03-29

(cobra to statsd bot) add ability to extract a numerical value and send a gauge event to statsd, with the --gauge option

## [9.1.6] - 2020-03-29

(ws cobra subscriber) use a Json::StreamWriter to write to std::cout, and save one std::string allocation for each message printed

## [9.1.5] - 2020-03-29

(docker) trim down docker image (300M -> 12M) / binary built without symbol and size optimization, and source code not copied over

## [9.1.4] - 2020-03-28

(jsoncpp) update bundled copy to version 1.9.3 (at sha 3beb37ea14aec1bdce1a6d542dc464d00f4a6cec)

## [9.1.3] - 2020-03-27

(docker) alpine docker build with release with debug info, and bundle ca-certificates

## [9.1.2] - 2020-03-26

(mac ssl) rename DarwinSSL -> SecureTransport (see this too -> https://github.com/curl/curl/issues/3733)

## [9.1.1] - 2020-03-26

(websocket) fix data race accessing _socket object without mutex protection when calling wakeUpFromPoll in WebSocketTransport.cpp

## [9.1.0] - 2020-03-26

(ixcobra) add explicit event types for handshake, authentication and subscription failure, and handle those by exiting in ws_cobra_subcribe and friends

## [9.0.3] - 2020-03-24

(ws connect) display statistics about how much time it takes to stop the connection

## [9.0.2] - 2020-03-24

(socket) works with unique_ptr<Socket> instead of shared_ptr<Socket> in many places

## [9.0.1] - 2020-03-24

(socket) selectInterrupt member is an unique_ptr instead of being a shared_ptr

## [9.0.0] - 2020-03-23

(websocket) reset per-message deflate codec everytime we connect to a server/client

## [8.3.4] - 2020-03-23

(websocket) fix #167, a long standing issue with sending empty messages with per-message deflate extension (and hopefully other zlib bug)

## [8.3.3] - 2020-03-22

(cobra to statsd) port to windows and add a unittest

## [8.3.2] - 2020-03-20

(websocket+tls) fix hang in tls handshake which could lead to ANR, discovered through unittesting.

## [8.3.1] - 2020-03-20

(cobra) CobraMetricsPublisher can be configure with an ix::CobraConfig + more unittest use SSL in server + client

## [8.3.0] - 2020-03-18

(websocket) Simplify ping/pong based heartbeat implementation

## [8.2.7] - 2020-03-17

(ws) ws connect gains a new option to set the interval at which to send pings
(ws) ws echo_server gains a new option (-p) to disable responding to pings with pongs

```
IXWebSocket$ ws connect --ping_interval 2 wss://echo.websocket.org
Type Ctrl-D to exit prompt...
Connecting to url: wss://echo.websocket.org
> ws_connect: connected
[2020-03-17 23:53:02.726] [info] Uri: /
[2020-03-17 23:53:02.726] [info] Headers:
[2020-03-17 23:53:02.727] [info] Connection: Upgrade
[2020-03-17 23:53:02.727] [info] Date: Wed, 18 Mar 2020 06:45:05 GMT
[2020-03-17 23:53:02.727] [info] Sec-WebSocket-Accept: 0gtqbxW0aVL/QI/ICpLFnRaiKgA=
[2020-03-17 23:53:02.727] [info] sec-websocket-extensions:
[2020-03-17 23:53:02.727] [info] Server: Kaazing Gateway
[2020-03-17 23:53:02.727] [info] Upgrade: websocket
[2020-03-17 23:53:04.894] [info] Received pong
[2020-03-17 23:53:06.859] [info] Received pong
[2020-03-17 23:53:08.881] [info] Received pong
[2020-03-17 23:53:10.848] [info] Received pong
[2020-03-17 23:53:12.898] [info] Received pong
[2020-03-17 23:53:14.865] [info] Received pong
[2020-03-17 23:53:16.890] [info] Received pong
[2020-03-17 23:53:18.853] [info] Received pong

[2020-03-17 23:53:19.388] [info]
ws_connect: connection closed: code 1000 reason Normal closure

[2020-03-17 23:53:19.502] [info] Received 208 bytes
[2020-03-17 23:53:19.502] [info] Sent 0 bytes
```

## [8.2.6] - 2020-03-16

(cobra to sentry bot + docker) default docker file uses mbedtls + ws cobra_to_sentry pass tls options to sentryClient.

## [8.2.5] - 2020-03-13

(cobra client) ws cobra subscribe resubscribe at latest position after being disconnected

## [8.2.4] - 2020-03-13

(cobra client) can subscribe with a position

## [8.2.3] - 2020-03-13

(cobra client) pass the message position to the subscription data callback

## [8.2.2] - 2020-03-12

(openssl tls backend) Fix a hand in OpenSSL when using TLS v1.3 ... by disabling TLS v1.3

## [8.2.1] - 2020-03-11

(cobra) IXCobraConfig struct has tlsOptions and per message deflate options

## [8.2.0] - 2020-03-11

(cobra) add IXCobraConfig struct to pass cobra config around

## [8.1.9] - 2020-03-09

(ws cobra_subscribe) add a --fluentd option to wrap a message in an enveloppe so that fluentd can recognize it

## [8.1.8] - 2020-03-02

(websocket server) fix regression with disabling zlib extension on the server side. If a client does not support this extension the server will handle it fine. We still need to figure out how to disable the option.

## [8.1.7] - 2020-02-26

(websocket) traffic tracker received bytes is message size while it should be wire size

## [8.1.6] - 2020-02-26

(ws_connect) display sent/received bytes statistics on exit

## [8.1.5] - 2020-02-23

(server) give thread name to some usual worker threads / unittest is broken !!

## [8.1.4] - 2020-02-22

(websocket server) fix regression from 8.1.2, where per-deflate message compression was always disabled

## [8.1.3] - 2020-02-21

(client + server) Fix #155 / http header parser should treat the space(s) after the : delimiter as optional. Fixing this bug made us discover that websocket sub-protocols are not properly serialiazed, but start with a ,

## [8.1.2] - 2020-02-18

(WebSocketServer) add option to disable deflate compression, exposed with the -x option to ws echo_server

## [8.1.1] - 2020-02-18

(ws cobra to statsd and sentry sender) exit if no messages are received for one minute, which is a sign that something goes wrong on the server side. That should be changed to be configurable in the future

## [8.1.0] - 2020-02-13

(http client + sentry minidump upload) Multipart stream closing boundary is invalid + mark some options as mandatory in the command line tools

## [8.0.7] - 2020-02-12

(build) remove the unused subtree which was causing some way of installing to break

## [8.0.6] - 2020-01-31

(snake) add an option to disable answering pongs as response to pings, to test cobra client behavior with hanged connections

## [8.0.5] - 2020-01-31

(IXCobraConnection) set a ping timeout of 90 seconds. If no pong messages are received as responses to ping for a while, give up and close the connection

## [8.0.4] - 2020-01-31

(cobra to sentry) remove noisy logging

## [8.0.3] - 2020-01-30

(ixcobra) check if we are authenticated in publishNext before trying to publish a message

## [8.0.2] - 2020-01-28

Extract severity level when emitting messages to sentry

## [8.0.1] - 2020-01-28

Fix bug #151 - If a socket connection is interrupted, calling stop() on the IXWebSocket object blocks until the next retry

## [8.0.0] - 2020-01-26

(SocketServer) add ability to bind on an ipv6 address

## [7.9.6] - 2020-01-22

(ws) add a dnslookup sub-command, to get the ip address of a remote host

## [7.9.5] - 2020-01-14

(windows) fix #144, get rid of stubbed/un-implemented windows schannel ssl backend

## [7.9.4] - 2020-01-12

(openssl + mbedssl) fix #140, can send large files with ws send over ssl / still broken with apple ssl

## [7.9.3] - 2020-01-10

(apple ssl) model write method after the OpenSSL one for consistency

## [7.9.2] - 2020-01-06

(apple ssl) unify read and write ssl utility code

## [7.9.1] - 2020-01-06

(websocket client) better error propagation when errors are detected while sending data
(ws send) detect failures to send big files, terminate in those cases and report error

## [7.9.0] - 2020-01-04

(ws send) add option (-x) to disable per message deflate compression

## [7.8.9] - 2020-01-04

(ws send + receive) handle all message types (ping + pong + fragment) / investigate #140

## [7.8.8] - 2019-12-28

(mbedtls) fix related to private key file parsing and initialization

## [7.8.6] - 2019-12-28

(ws cobra to sentry/statsd) fix for handling null events properly for empty queues + use queue to send data to statsd

## [7.8.5] - 2019-12-28

(ws cobra to sentry) handle null events for empty queues

## [7.8.4] - 2019-12-27

(ws cobra to sentry) game is picked in a fair manner, so that all games get the same share of sent events

## [7.8.3] - 2019-12-27

(ws cobra to sentry) refactor queue related code into a class

## [7.8.2] - 2019-12-25

(ws cobra to sentry) bound the queue size used to hold up cobra messages before they are sent to sentry. Default queue size is a 100 messages. Without such limit the program runs out of memory when a subscriber receive a lot of messages that cannot make it to sentry

## [7.8.1] - 2019-12-25

(ws client) use correct compilation defines so that spdlog is not used as a header only library (reduce binary size and increase compilation speed)

## [7.8.0] - 2019-12-24

(ws client) all commands use spdlog instead of std::cerr or std::cout for logging

## [7.6.5] - 2019-12-24

(cobra client) send a websocket ping every 30s to keep the connection opened

## [7.6.4] - 2019-12-22

(client) error handling, quote url in error case when failing to parse one
(ws) ws_cobra_publish: register callbacks before connecting
(doc) mention mbedtls in supported ssl server backend

## [7.6.3] - 2019-12-20

(tls) add a simple description of the TLS configuration routine for debugging

## [7.6.2] - 2019-12-20

(mbedtls) correct support for using own certificate and private key

## [7.6.1] - 2019-12-20

(ws commands) in websocket proxy, disable automatic reconnections + in Dockerfile, use alpine 3.11

## [7.6.0] - 2019-12-19

(cobra) Add TLS options to all cobra commands and classes. Add example to the doc.

## [7.5.8] - 2019-12-18

(cobra-to-sentry) capture application version from device field

## [7.5.7] - 2019-12-18

(tls) Experimental TLS server support with mbedtls (windows) + process cert tlsoption (client + server)

## [7.5.6] - 2019-12-18

(tls servers) Make it clear that apple ssl and mbedtls backends do not support SSL in server mode

## [7.5.5] - 2019-12-17

(tls options client) TLSOptions struct _validated member should be initialized to false

## [7.5.4] - 2019-12-16

(websocket client) improve the error message when connecting to a non websocket server

Before:

```
Connection error: Got bad status connecting to example.com:443, status: 200, HTTP Status line: HTTP/1.1 200 OK
```

After:

```
Connection error: Expecting status 101 (Switching Protocol), got 200 status connecting to example.com:443, HTTP Status line: HTTP/1.1 200 OK
```

## [7.5.3] - 2019-12-12

(server) attempt at fixing #131 by using blocking writes in server mode

## [7.5.2] - 2019-12-11

(ws) cobra to sentry - created events with sentry tags based on tags present in the cobra messages

## [7.5.1] - 2019-12-06

(mac) convert SSL errors to utf8

## [7.5.0] - 2019-12-05

- (ws) cobra to sentry. Handle Error 429 Too Many Requests and politely wait before sending more data to sentry.

In the example below sentry we are sending data too fast, sentry asks us to slow down which we do. Notice how the sent count stop increasing, while we are waiting for 41 seconds.

```
[2019-12-05 15:50:33.759] [info] messages received 2449 sent 3
[2019-12-05 15:50:34.759] [info] messages received 5533 sent 7
[2019-12-05 15:50:35.759] [info] messages received 8612 sent 11
[2019-12-05 15:50:36.759] [info] messages received 11562 sent 15
[2019-12-05 15:50:37.759] [info] messages received 14410 sent 19
[2019-12-05 15:50:38.759] [info] messages received 17236 sent 23
[2019-12-05 15:50:39.282] [error] Error sending data to sentry: 429
[2019-12-05 15:50:39.282] [error] Body: {"exception":[{"stacktrace":{"frames":[{"filename":"WorldScene.lua","function":"WorldScene.lua:1935","lineno":1958},{"filename":"WorldScene.lua","function":"onUpdate_WorldCam","lineno":1921},{"filename":"WorldMapTile.lua","function":"__index","lineno":239}]},"value":"noisytypes: Attempt to call nil(nil,2224139838)!"}],"platform":"python","sdk":{"name":"ws","version":"1.0.0"},"tags":[["game","niso"],["userid","107638363"],["environment","live"]],"timestamp":"2019-12-05T23:50:39Z"}

[2019-12-05 15:50:39.282] [error] Response: {"error_name":"rate_limit","error":"Creation of this event was denied due to rate limiting"}
[2019-12-05 15:50:39.282] [warning] Error 429 - Too Many Requests. ws will sleep and retry after 41 seconds
[2019-12-05 15:50:39.760] [info] messages received 18839 sent 25
[2019-12-05 15:50:40.760] [info] messages received 18839 sent 25
[2019-12-05 15:50:41.760] [info] messages received 18839 sent 25
[2019-12-05 15:50:42.761] [info] messages received 18839 sent 25
[2019-12-05 15:50:43.762] [info] messages received 18839 sent 25
[2019-12-05 15:50:44.763] [info] messages received 18839 sent 25
[2019-12-05 15:50:45.768] [info] messages received 18839 sent 25
```

## [7.4.5] - 2019-12-03

- (ws) #125 / fix build problem when jsoncpp is not installed locally

## [7.4.4] - 2019-12-03

- (ws) #125 / cmake detects an already installed jsoncpp and will try to use this one if present

## [7.4.3] - 2019-12-03

- (http client) use std::unordered_map instead of std::map for HttpParameters and HttpFormDataParameters class aliases

## [7.4.2] - 2019-12-02

- (client) internal IXDNSLookup class requires a valid cancellation request function callback to be passed in

## [7.4.1] - 2019-12-02

- (client) fix an overflow in the exponential back off code

## [7.4.0] - 2019-11-25

- (http client) Add support for multipart HTTP POST upload
- (ixsentry) Add support for uploading a minidump to sentry

## [7.3.5] - 2019-11-20

- On Darwin SSL, add ability to skip peer verification.

## [7.3.4] - 2019-11-20

- 32-bits compile fix, courtesy of @fcojavmc

## [7.3.1] - 2019-11-16

- ws proxy_server / remote server close not forwarded to the client

## [7.3.0] - 2019-11-15

- New ws command: `ws proxy_server`.

## [7.2.2] - 2019-11-01

- Tag a release + minor reformating.

## [7.2.1] - 2019-10-26

- Add unittest to IXSentryClient to lua backtrace parsing code

## [7.2.0] - 2019-10-24

- Add cobra_metrics_to_redis sub-command to create streams for each cobra metric event being received.

## [7.1.0] - 2019-10-13

- Add client support for websocket subprotocol. Look for the new addSubProtocol method for details.

## [7.0.0] - 2019-10-01

- TLS support in server code, only implemented for the OpenSSL SSL backend for now.

## [6.3.4] - 2019-09-30

- all ws subcommands propagate tls options to servers (unimplemented) or ws or http client (implemented) (contributed by Matt DeBoer)

## [6.3.3] - 2019-09-30

- ws has a --version option

## [6.3.2] - 2019-09-29

- (http + websocket clients) can specify cacert and some other tls options (not implemented on all backend). This makes it so that server certs can finally be validated on windows.

## [6.3.1] - 2019-09-29

- Add ability to use OpenSSL on apple platforms.

## [6.3.0] - 2019-09-28

- ixcobra / fix crash in CobraConnection::publishNext when the queue is empty + handle CobraConnection_PublishMode_Batch in CobraMetricsThreadedPublisher

## [6.2.9] - 2019-09-27

- mbedtls fixes / the unittest now pass on macOS, and hopefully will on Windows/AppVeyor as well.

## [6.2.8] - 2019-09-26

- Http server: add options to ws https to redirect all requests to a given url. POST requests will get a 200 and an empty response.

```
ws httpd -L --redirect_url https://www.google.com
```

## [6.2.7] - 2019-09-25

- Stop having ws send subcommand send a binary message in text mode, which would cause error in `make ws_test` shell script test.

## [6.2.6] - 2019-09-24

- Fix 2 race conditions detected with TSan, one in CobraMetricsPublisher::push and another one in WebSocketTransport::sendData (that one was bad).

## [6.2.5] - 2019-09-23

- Add simple Redis Server which is only capable of doing publish / subscribe. New ws redis_server sub-command to use it. The server is used in the unittest, so that we can run on CI in environment where redis isn not available like github actions env.

## [6.2.4] - 2019-09-22

- Add options to configure TLS ; contributed by Matt DeBoer. Only implemented for OpenSSL TLS backend for now.

## [6.2.3] - 2019-09-21

- Fix crash in the Linux unittest in the HTTP client code, in Socket::readBytes
- Cobra Metrics Publisher code returns the message id of the message that got published, to be used to validated that it got sent properly when receiving an ack.

## [6.2.2] - 2019-09-19

- In DNS lookup code, make sure the weak pointer we use lives through the expected scope (if branch)

## [6.2.1] - 2019-09-17

- On error while doing a client handshake, additionally display port number next to the host name

## [6.2.0] - 2019-09-09

- websocket and http server: server does not close the bound client socket in many cases
- improve some websocket error messages
- add a utility function with unittest to parse status line and stop using scanf which triggers warnings on Windows
- update ws CLI11 (our command line argument parsing library) to the latest, which fix a compiler bug about optional

## [6.1.0] - 2019-09-08

- move poll wrapper on top of select (only used on Windows) to the ix namespace

## [6.0.1] - 2019-09-05

- add cobra metrics publisher + server unittest
- add cobra client + server unittest
- ws snake (cobra simple server) add basic support for unsubscription + subscribe send the proper subscription data + redis client subscription can be cancelled
- IXCobraConnection / pdu handlers can crash if they receive json data which is not an object

## [6.0.0] - 2019-09-04

- all client autobahn test should pass !
- zlib/deflate has a bug with windowsbits == 8, so we silently upgrade it to 9/ (fix autobahn test 13.X which uses 8 for the windows size)

## [5.2.0] - 2019-09-04

- Fragmentation: for sent messages which are compressed, the continuation fragments should not have the rsv1 bit set (fix all autobahn tests for zlib compression 12.X)
- Websocket Server / do a case insensitive string search when looking for an Upgrade header whose value is websocket. (some client use WebSocket with some upper-case characters)

## [5.1.9] - 2019-09-03

- ws autobahn / report progress with spdlog::info to get timing info
- ws autobahn / use condition variables for stopping test case + add more logging on errors

## [5.1.8] - 2019-09-03

- Per message deflate/compression: handle fragmented messages (fix autobahn test: 12.1.X and probably others)

## [5.1.7] - 2019-09-03

- Receiving invalid UTF-8 TEXT message should fail and close the connection (fix remaining autobahn test: 6.X UTF-8 Handling)

## [5.1.6] - 2019-09-03

- Sending invalid UTF-8 TEXT message should fail and close the connection (fix remaining autobahn test: 6.X UTF-8 Handling)
- Fix failing unittest which was sending binary data in text mode with WebSocket::send to call properly call WebSocket::sendBinary instead.
- Validate that the reason is proper utf-8. (fix autobahn test 7.5.1)
- Validate close codes. Autobahn 7.9.*

## [5.1.5] - 2019-09-03

Framentation: data and continuation blocks received out of order (fix autobahn test: 5.9 through 5.20 Fragmentation)

## [5.1.4] - 2019-09-03

Sending invalid UTF-8 TEXT message should fail and close the connection (fix **tons** of autobahn test: 6.X UTF-8 Handling)

## [5.1.3] - 2019-09-03

Message type (TEXT or BINARY) is invalid for received fragmented messages (fix autobahn test: 5.3 through 5.8 Fragmentation)

## [5.1.2] - 2019-09-02

Ping and Pong messages cannot be fragmented (fix autobahn test: 5.1 and 5.2 Fragmentation)

## [5.1.1] - 2019-09-01

Close connections when reserved bits are used (fix autobahn test: 3.X Reserved Bits)

## [5.1.0] - 2019-08-31

- ws autobahn / Add code to test websocket client compliance with the autobahn test-suite
- add utf-8 validation code, not hooked up properly yet
- Ping received with a payload too large (> 125 bytes) trigger a connection closure
- cobra / add tracking about published messages
- cobra / publish returns a message id, that can be used when
- cobra / new message type in the message received handler when publish/ok is received (can be used to implement an ack system).

## [5.0.9] - 2019-08-30

- User-Agent header is set when not specified.
- New option to cap the max wait between reconnection attempts. Still default to 10s. (setMaxWaitBetweenReconnectionRetries).

```
ws connect --max_wait 5000 ws://example.com # will only wait 5 seconds max between reconnection attempts
```

## [5.0.7] - 2019-08-23
- WebSocket: add new option to pass in extra HTTP headers when connecting.
- `ws connect` add new option (-H, works like [curl](https://stackoverflow.com/questions/356705/how-to-send-a-header-using-a-http-request-through-a-curl-call)) to pass in extra HTTP headers when connecting

If you run against `ws echo_server` you will see the headers being received printed in the terminal.
```
ws connect -H "foo: bar" -H "baz: buz" ws://127.0.0.1:8008
```

- CobraConnection: sets a unique id field for all messages sent to [cobra](https://github.com/machinezone/cobra).
- CobraConnection: sets a counter as a field for each event published.

## [5.0.6] - 2019-08-22
- Windows: silly compile error (poll should be in the global namespace)

## [5.0.5] - 2019-08-22
- Windows: use select instead of WSAPoll, through a poll wrapper

## [5.0.4] - 2019-08-20
- Windows build fixes (there was a problem with the use of ::poll that has a different name on Windows (WSAPoll))

## [5.0.3] - 2019-08-14
- CobraMetricThreadedPublisher _enable flag is an atomic, and CobraMetricsPublisher is enabled by default

## [5.0.2] - 2019-08-01
- ws cobra_subscribe has a new -q (quiet) option
- ws cobra_subscribe knows to and display msg stats (count and # of messages received per second)
- ws cobra_subscribe, cobra_to_statsd and cobra_to_sentry commands have a new option, --filter to restrict the events they want to receive

## [5.0.1] - 2019-07-25
- ws connect command has a new option to send in binary mode (still default to text)
- ws connect command has readline history thanks to libnoise-cpp. Now ws connect one can use using arrows to lookup previous sent messages and edit them

## [5.0.0] - 2019-06-23
### Changed
- New HTTP server / still very early. ws gained a new command, httpd can run a simple webserver serving local files.
- IXDNSLookup. Uses weak pointer + smart_ptr + shared_from_this instead of static sets + mutex to handle object going away before dns lookup has resolved
- cobra_to_sentry / backtraces are reversed and line number is not extracted correctly
- mbedtls and zlib are searched with find_package, and we use the vendored version if nothing is found
- travis CI uses g++ on Linux

## [4.0.0] - 2019-06-09
### Changed
- WebSocket::send() sends message in TEXT mode by default
- WebSocketMessage sets a new binary field, which tells whether the received incoming message is binary or text
- WebSocket::send takes a third arg, binary which default to true (can be text too)
- WebSocket callback only take one object, a const ix::WebSocketMessagePtr& msg
- Add explicit WebSocket::sendBinary method
- New headers + WebSocketMessage class to hold message data, still not used across the board
- Add test/compatibility folder with small servers and clients written in different languages and different libraries to test compatibility.
- ws echo_server has a -g option to print a greeting message on connect
- IXSocketMbedTLS: better error handling in close and connect

## [3.1.2] - 2019-06-06
### Added
- ws connect has a -x option to disable per message deflate
- Add WebSocket::disablePerMessageDeflate() option.

## [3.0.0] - 2019-06-xx
### Changed
- TLS, aka SSL works on Windows (websocket and http clients)
- ws command line tool build on Windows
- Async API for HttpClient
- HttpClient API changed to use shared_ptr for response and request
