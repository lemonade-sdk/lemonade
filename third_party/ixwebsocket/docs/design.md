## Implementation details

### Per Message Deflate compression.

The per message deflate compression option is supported. It can lead to very nice bandbwith savings (20x !) if your messages are similar, which is often the case for example for chat applications. All features of the spec should be supported.

### TLS/SSL

Connections can be optionally secured and encrypted with TLS/SSL when using a wss:// endpoint, or using normal un-encrypted socket with ws:// endpoints. AppleSSL is used on iOS and macOS, OpenSSL and mbedTLS can be used on Android, Linux and Windows.

If you are using OpenSSL, try to be on a version higher than 1.1.x as there there are thread safety problems with 1.0.x.

### Polling and background thread work

No manual polling to fetch data is required. Data is sent and received instantly by using a background thread for receiving data and the select [system](http://man7.org/linux/man-pages/man2/select.2.html) call to be notified by the OS of incoming data. No timeout is used for select so that the background thread is only woken up when data is available, to optimize battery life. This is also the recommended way of using select according to the select tutorial, section [select law](https://linux.die.net/man/2/select_tut). Read and Writes to the socket are non blocking. Data is sent right away and not enqueued by writing directly to the socket, which is [possible](https://stackoverflow.com/questions/1981372/are-parallel-calls-to-send-recv-on-the-same-socket-valid) since system socket implementations allow concurrent read/writes.

### Automatic reconnection

If the remote end (server) breaks the connection, the code will try to perpetually reconnect, by using an exponential backoff strategy, capped at one retry every 10 seconds. This behavior can be disabled.

### Large messages

Large frames are broken up into smaller chunks or messages to avoid filling up the os tcp buffers, which is permitted thanks to WebSocket [fragmentation](https://tools.ietf.org/html/rfc6455#section-5.4). Messages up to 1G were sent and received succesfully.

### Testing

The library has an interactive tool which is handy for testing compatibility ith other libraries. We have tested our client against Python, Erlang, Node.js, and C++ websocket server libraries.

The unittest tries to be comprehensive, and has been running on multiple platforms, with different sanitizers such as a thread sanitizer to catch data races or the undefined behavior sanitizer.

The regression test is running after each commit on github actions for multiple configurations.

## Limitations

* On some configuration (mostly Android) certificate validation needs to be setup so that SocketTLSOptions.caFile point to a pem file, such as the one distributed by Firefox. Unless that setup is done connecting to a wss endpoint will display an error. With mbedtls the message will contain `error in handshake : X509 - Certificate verification failed, e.g. CRL, CA or signature check failed`.
* Automatic reconnection works at the TCP socket level, and will detect remote end disconnects. However, if the device/computer network become unreachable (by turning off wifi), it is quite hard to reliably and timely detect it at the socket level using `recv` and `send` error codes. [Here](https://stackoverflow.com/questions/14782143/linux-socket-how-to-detect-disconnected-network-in-a-client-program) is a good discussion on the subject. This behavior is consistent with other runtimes such as node.js. One way to detect a disconnected device with low level C code is to do a name resolution with DNS but this can be expensive. Mobile devices have good and reliable API to do that.
* The server code is using select to detect incoming data, and creates one OS thread per connection. This is not as scalable as strategies using epoll or kqueue.

## C++ code organization

Here is a simplistic diagram which explains how the code is structured in term of class/modules.

```
+-----------------------+ --- Public
|                       | Start the receiving Background thread. Auto reconnection. Simple websocket Ping.
|  IXWebSocket          | Interface used by C++ test clients. No IX dependencies.
|                       |
+-----------------------+
|                       |
|  IXWebSocketServer    | Run a server and give each connections its own WebSocket object.
|                       | Each connection is handled in a new OS thread.
|                       |
+-----------------------+ --- Private
|                       |
|  IXWebSocketTransport | Low level websocket code, framing, managing raw socket. Adapted from easywsclient.
|                       |
+-----------------------+
|                       |
|  IXWebSocketHandshake | Establish the connection between client and server.
|                       |
+-----------------------+
|                       |
|  IXWebSocket          | ws://  Unencrypted Socket handler
|  IXWebSocketAppleSSL  | wss:// TLS encrypted Socket AppleSSL handler. Used on iOS and macOS
|  IXWebSocketOpenSSL   | wss:// TLS encrypted Socket OpenSSL handler.  Used on Android and Linux
|                       |                                               Can be used on macOS too.
+-----------------------+
|                       |
|  IXSocketConnect      | Connect to the remote host (client).
|                       |
+-----------------------+
|                       |
|  IXDNSLookup          | Does DNS resolution asynchronously so that it can be interrupted.
|                       |
+-----------------------+
```
