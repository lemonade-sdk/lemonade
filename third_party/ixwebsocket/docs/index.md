## Hello world

IXWebSocket is a C++ library for WebSocket client and server development. It has minimal dependencies (no boost), is very simple to use and support everything you'll likely need for websocket dev (SSL, deflate compression, compiles on most platforms, etc...). HTTP client and server code is also available, but it hasn't received as much testing.

It is been used on big mobile video game titles sending and receiving tons of messages since 2017 (iOS and Android). It was tested on macOS, iOS, Linux, Android, Windows and FreeBSD. Note that the MinGW compiler is not supported at this point. Two important design goals are simplicity and correctness.

A bad security bug affecting users compiling with SSL enabled and OpenSSL as the backend was just fixed in newly released version 11.0.0. Please upgrade ! (more details in the [https://github.com/machinezone/IXWebSocket/pull/250](PR).

```cpp
/*
 *  main.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2020 Machine Zone, Inc. All rights reserved.
 *
 *  Super simple standalone example. See ws folder, unittest and doc/usage.md for more.
 *
 *  On macOS
 *  $ mkdir -p build ; (cd build ; cmake -DUSE_TLS=1 .. ; make -j ; make install)
 *  $ clang++ --std=c++11 --stdlib=libc++ main.cpp -lixwebsocket -lz -framework Security -framework Foundation
 *  $ ./a.out
 *
 *  Or use cmake -DBUILD_DEMO=ON option for other platforms
 */

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXUserAgent.h>
#include <iostream>

int main()
{
    // Required on Windows
    ix::initNetSystem();

    // Our websocket object
    ix::WebSocket webSocket;

    // Connect to a server with encryption
    // See https://machinezone.github.io/IXWebSocket/usage/#tls-support-and-configuration
    std::string url("wss://echo.websocket.org");
    webSocket.setUrl(url);

    std::cout << "Connecting to " << url << "..." << std::endl;

    // Setup a callback to be fired (in a background thread, watch out for race conditions !)
    // when a message or an event (open, close, error) is received
    webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg)
        {
            if (msg->type == ix::WebSocketMessageType::Message)
            {
                std::cout << "received message: " << msg->str << std::endl;
                std::cout << "> " << std::flush;
            }
            else if (msg->type == ix::WebSocketMessageType::Open)
            {
                std::cout << "Connection established" << std::endl;
                std::cout << "> " << std::flush;
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                // Maybe SSL is not configured properly
                std::cout << "Connection error: " << msg->errorInfo.reason << std::endl;
                std::cout << "> " << std::flush;
            }
        }
    );

    // Now that our callback is setup, we can start our background thread and receive messages
    webSocket.start();

    // Send a message to the server (default to TEXT mode)
    webSocket.send("hello world");

    // Display a prompt
    std::cout << "> " << std::flush;

    std::string text;
    // Read text from the console and send messages in text mode.
    // Exit with Ctrl-D on Unix or Ctrl-Z on Windows.
    while (std::getline(std::cin, text))
    {
        webSocket.send(text);
        std::cout << "> " << std::flush;
    }

    return 0;
}
```

Interested? Go read the [docs](https://machinezone.github.io/IXWebSocket/)! If things don't work as expected, please create an issue on GitHub, or even better a pull request if you know how to fix your problem.

IXWebSocket is actively being developed, check out the [changelog](https://machinezone.github.io/IXWebSocket/CHANGELOG/) to know what's cooking. If you are looking for a real time messaging service (the chat-like 'server' your websocket code will talk to) with many features such as history, backed by Redis, look at [cobra](https://github.com/machinezone/cobra).

IXWebSocket client code is autobahn compliant beginning with the 6.0.0 version. See the current [test results](https://bsergean.github.io/autobahn/reports/clients/index.html). Some tests are still failing in the server code.

Starting with the 11.0.8 release, IXWebSocket should be fully C++11 compatible.

## Users

If your company or project is using this library, feel free to open an issue or PR to amend this list.

- [Machine Zone](https://www.mz.com)
- [Tokio](https://gitlab.com/HCInk/tokio), a discord library focused on audio playback with node bindings.
- [libDiscordBot](https://github.com/tostc/libDiscordBot/tree/master), an easy to use Discord-bot framework.
- [gwebsocket](https://github.com/norrbotten/gwebsocket), a websocket (lua) module for Garry's Mod
- [DisCPP](https://github.com/DisCPP/DisCPP), a simple but feature rich Discord API wrapper
- [discord.cpp](https://github.com/luccanunes/discord.cpp), a discord library for making bots
- [Teleport](http://teleportconnect.com/), Teleport is your own personal remote robot avatar

## Alternative libraries

There are plenty of great websocket libraries out there, which might work for you. Here are a couple of serious ones.

* [websocketpp](https://github.com/zaphoyd/websocketpp) - C++
* [beast](https://github.com/boostorg/beast) - C++
* [libwebsockets](https://libwebsockets.org/) - C
* [µWebSockets](https://github.com/uNetworking/uWebSockets) - C
* [wslay](https://github.com/tatsuhiro-t/wslay) - C

[uvweb](https://github.com/bsergean/uvweb) is a library written by the IXWebSocket author which is built on top of [uvw](https://github.com/skypjack/uvw), which is a C++ wrapper for [libuv](https://libuv.org/). It has more dependencies and does not support SSL at this point, but it can be used to open multiple connections within a single OS thread thanks to libuv.

To check the performance of a websocket library, you can look at the [autoroute](https://github.com/bsergean/autoroute) project.

## Continuous Integration

| OS                | TLS               | Sanitizer         | Status            |
|-------------------|-------------------|-------------------|-------------------|
| Linux             | OpenSSL           | None              | [![Build2][1]][0] |
| macOS             | Secure Transport  | Thread Sanitizer  | [![Build2][2]][0] |
| macOS             | OpenSSL           | Thread Sanitizer  | [![Build2][3]][0] |
| macOS             | MbedTLS           | Thread Sanitizer  | [![Build2][4]][0] |
| Windows           | Disabled          | None              | [![Build2][5]][0] |
| UWP               | Disabled          | None              | [![Build2][6]][0] |
| Linux             | OpenSSL           | Address Sanitizer | [![Build2][7]][0] |
| Mingw             | Disabled          | None              | [![Build2][8]][0] |

* ASAN fails on Linux because of a known problem, we need a
* Some tests are disabled on Windows/UWP because of a pathing problem
* TLS and ZLIB are disabled on Windows/UWP because enabling make the CI run takes a lot of time, for setting up vcpkg.

[0]: https://github.com/machinezone/IXWebSocket
[1]: https://github.com/machinezone/IXWebSocket/workflows/linux/badge.svg
[2]: https://github.com/machinezone/IXWebSocket/workflows/mac_tsan_sectransport/badge.svg
[3]: https://github.com/machinezone/IXWebSocket/workflows/mac_tsan_openssl/badge.svg
[4]: https://github.com/machinezone/IXWebSocket/workflows/mac_tsan_mbedtls/badge.svg
[5]: https://github.com/machinezone/IXWebSocket/workflows/windows/badge.svg
[6]: https://github.com/machinezone/IXWebSocket/workflows/uwp/badge.svg
[7]: https://github.com/machinezone/IXWebSocket/workflows/linux_asan/badge.svg
[8]: https://github.com/machinezone/IXWebSocket/workflows/unittest_windows_gcc/badge.svg
