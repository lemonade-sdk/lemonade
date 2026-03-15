# Examples

The [*ws*](https://github.com/machinezone/IXWebSocket/tree/master/ws) folder countains many interactive programs for chat, [file transfers](https://github.com/machinezone/IXWebSocket/blob/master/ws/ws_send.cpp), [curl like](https://github.com/machinezone/IXWebSocket/blob/master/ws/ws_http_client.cpp) http clients, demonstrating client and server usage.

## Windows note

To use the network system on Windows, you need to initialize it once with *WSAStartup()* and clean it up with *WSACleanup()*. We have helpers for that which you can use, see below. This init would typically take place in your main function.

```cpp
#include <ixwebsocket/IXNetSystem.h>

int main()
{
    ix::initNetSystem();

    ...

    ix::uninitNetSystem();
    return 0;
}
```

## WebSocket client API

```cpp
#include <ixwebsocket/IXWebSocket.h>

...

// Our websocket object
ix::WebSocket webSocket;

std::string url("ws://localhost:8080/");
webSocket.setUrl(url);

// Optional heart beat, sent every 45 seconds when there is not any traffic
// to make sure that load balancers do not kill an idle connection.
webSocket.setPingInterval(45);

// Per message deflate connection is not enabled by default. You can tweak its parameters, enable or disable it with
webSocket.enablePerMessageDeflate();
webSocket.disablePerMessageDeflate();

// Setup a callback to be fired when a message or an event (open, close, error) is received
webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg)
    {
        if (msg->type == ix::WebSocketMessageType::Message)
        {
            std::cout << msg->str << std::endl;
        }
    }
);

// Now that our callback is setup, we can start our background thread and receive messages
webSocket.start();

// Send a message to the server (default to TEXT mode)
webSocket.send("hello world");

// The message can be sent in BINARY mode (useful if you send MsgPack data for example)
webSocket.sendBinary("some serialized binary data");

// ... finally ...

// Stop the connection
webSocket.stop()
```

### Sending messages

`WebSocketSendInfo result = websocket.send("foo")` will send a message.

If the connection was closed, sending will fail, and the success field of the result object  will be set to false. There could also be a compression error in which case the compressError field will be set to true. The payloadSize field and wireSize fields will tell you respectively how much bytes the message weight, and how many bytes were sent on the wire (potentially compressed + counting the message header (a few bytes).

There is an optional progress callback that can be passed in as the second argument. If a message is large it will be fragmented into chunks which will be sent independantly. Everytime the we can write a fragment into the OS network cache, the callback will be invoked. If a user wants to cancel a slow send, false should be returned from within the callback.

Here is an example code snippet copied from the ws send sub-command. Each fragment weights 32K, so the total integer is the wireSize divided by 32K. As an example if you are sending 32M of data, uncompressed, total will be 1000. current will be set to 0 for the first fragment, then 1, 2 etc...

```
auto result =
    _webSocket.sendBinary(serializedMsg, [this, throttle](int current, int total) -> bool {
        spdlog::info("ws_send: Step {} out of {}", current + 1, total);

        if (throttle)
        {
            std::chrono::duration<double, std::milli> duration(10);
            std::this_thread::sleep_for(duration);
        }

        return _connected;
    });
```

The `send()` and `sendText()` methods check that the string contains only valid UTF-8 characters. If you know that the string is a valid UTF-8 string you can skip that step and use the `sendUtf8Text` method instead.

With the IXWebSocketSendData overloads of `sendUtf8Text` and `sendBinary` it is possible to not only send std::string but also `std::vector<char>`, `std::vector<uint8_t>` and `char*`.

```
std::vector<uint8_t> data({1, 2, 3, 4});
auto result = webSocket.sendBinary(data);

const char* text = "Hello World!";
result = webSocket.sendUtf8Text(IXWebSocketSendData(text, strlen(text)));
```

### ReadyState

`getReadyState()` returns the state of the connection. There are 4 possible states.

1. ReadyState::Connecting - The connection is not yet open.
2. ReadyState::Open       - The connection is open and ready to communicate.
3. ReadyState::Closing    - The connection is in the process of closing.
4. ReadyState::Closed     - The connection is closed or could not be opened.

### Open and Close notifications

The onMessage event will be fired when the connection is opened or closed. This is similar to the [JavaScript browser API](https://developer.mozilla.org/en-US/docs/Web/API/WebSocket), which has `open` and `close` events notification that can be registered with the browser `addEventListener`.

```cpp
webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg)
    {
        if (msg->type == ix::WebSocketMessageType::Open)
        {
            std::cout << "send greetings" << std::endl;

            // Headers can be inspected (pairs of string/string)
            std::cout << "Handshake Headers:" << std::endl;
            for (auto it : msg->headers)
            {
                std::cout << it.first << ": " << it.second << std::endl;
            }
        }
        else if (msg->type == ix::WebSocketMessageType::Close)
        {
            std::cout << "disconnected" << std::endl;

            // The server can send an explicit code and reason for closing.
            // This data can be accessed through the closeInfo object.
            std::cout << msg->closeInfo.code << std::endl;
            std::cout << msg->closeInfo.reason << std::endl;
        }
    }
);
```

### Error notification

A message will be fired when there is an error with the connection. The message type will be `ix::WebSocketMessageType::Error`. Multiple fields will be available on the event to describe the error.

```cpp
webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg)
    {
        if (msg->type == ix::WebSocketMessageType::Error)
        {
            std::stringstream ss;
            ss << "Error: "         << msg->errorInfo.reason      << std::endl;
            ss << "#retries: "      << msg->errorInfo.retries     << std::endl;
            ss << "Wait time(ms): " << msg->errorInfo.wait_time   << std::endl;
            ss << "HTTP Status: "   << msg->errorInfo.http_status << std::endl;
            std::cout << ss.str() << std::endl;
        }
    }
);
```

### start, stop

1. `websocket.start()` connect to the remote server and starts the message receiving background thread.
2. `websocket.stop()` disconnect from the remote server and closes the background thread.

### Configuring the remote url

The url can be set and queried after a websocket object has been created. You will have to call `stop` and `start` if you want to disconnect and connect to that new url.

```cpp
std::string url("wss://example.com");
websocket.configure(url);
```

### Ping/Pong support

Ping/pong messages are used to implement keep-alive. 2 message types exists to identify ping and pong messages. Note that when a ping message is received, a pong is instantly send back as requested by the WebSocket spec.

```cpp
webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg)
    {
        if (msg->type == ix::WebSocketMessageType::Ping ||
            msg->type == ix::WebSocketMessageType::Pong)
        {
            std::cout << "pong data: " << msg->str << std::endl;
        }
    }
);
```

A ping message can be sent to the server, with an optional data string.

```cpp
websocket.ping("ping data, optional (empty string is ok): limited to 125 bytes long");
```

### Heartbeat.

You can configure an optional heart beat / keep-alive, sent every 45 seconds
when there is no any traffic to make sure that load balancers do not kill an
idle connection.

```cpp
webSocket.setPingInterval(45);
```

### Supply extra HTTP headers.

You can set extra HTTP headers to be sent during the WebSocket handshake.

```cpp
WebSocketHttpHeaders headers;
headers["foo"] = "bar";
webSocket.setExtraHeaders(headers);
```

### Subprotocols

You can specify subprotocols to be set during the WebSocket handshake. For more info you can refer to [this doc](https://hpbn.co/websocket/#subprotocol-negotiation).

```cpp
webSocket.addSubprotocol("appProtocol-v1");
webSocket.addSubprotocol("appProtocol-v2");
```

The protocol that the server did accept is available in the open info `protocol` field.

```cpp
std::cout << "protocol: " << msg->openInfo.protocol << std::endl;
```

### Automatic reconnection

Automatic reconnection kicks in when the connection is disconnected without the user consent. This feature is on by default and can be turned off.

```cpp
webSocket.enableAutomaticReconnection();  // turn on
webSocket.disableAutomaticReconnection(); // turn off
bool enabled = webSocket.isAutomaticReconnectionEnabled(); // query state
```

The technique to calculate wait time is called [exponential
backoff](https://docs.aws.amazon.com/general/latest/gr/api-retries.html). Here
are the default waiting times between attempts (from connecting with `ws connect ws://foo.com`)

```
> Connection error: Got bad status connecting to foo.com, status: 301, HTTP Status line: HTTP/1.1 301 Moved Permanently

#retries: 1
Wait time(ms): 100
#retries: 2
Wait time(ms): 200
#retries: 3
Wait time(ms): 400
#retries: 4
Wait time(ms): 800
#retries: 5
Wait time(ms): 1600
#retries: 6
Wait time(ms): 3200
#retries: 7
Wait time(ms): 6400
#retries: 8
Wait time(ms): 10000
```

The waiting time is capped by default at 10s between 2 attempts, but that value
can be changed and queried. The minimum waiting time can also be set.

```cpp
webSocket.setMaxWaitBetweenReconnectionRetries(5 * 1000); // 5000ms = 5s
uint32_t m = webSocket.getMaxWaitBetweenReconnectionRetries();

webSocket.setMinWaitBetweenReconnectionRetries(1000); // 1000ms = 1s
uint32_t m = webSocket.getMinWaitBetweenReconnectionRetries();
```

## Handshake timeout

You can control how long to wait until timing out while waiting for the websocket handshake to be performed.

```
int handshakeTimeoutSecs = 1;
setHandshakeTimeout(handshakeTimeoutSecs);
```

## WebSocket server API

### Legacy api

This api was actually changed to take a weak_ptr<WebSocket> as the first argument to setOnConnectionCallback ; previously it would take a shared_ptr<WebSocket> which was creating cycles and then memory leaks problems.

```cpp
#include <ixwebsocket/IXWebSocketServer.h>

...

// Run a server on localhost at a given port.
// Bound host name, max connections and listen backlog can also be passed in as parameters.
int port = 8008;
std::string host("127.0.0.1"); // If you need this server to be accessible on a different machine, use "0.0.0.0"
ix::WebSocketServer server(port, host);

server.setOnConnectionCallback(
    [&server](std::weak_ptr<WebSocket> webSocket,
              std::shared_ptr<ConnectionState> connectionState)
    {
        std::cout << "Remote ip: " << connectionState->remoteIp << std::endl;

        auto ws = webSocket.lock();
        if (ws)
        {
            ws->setOnMessageCallback(
                [webSocket, connectionState, &server](const ix::WebSocketMessagePtr msg)
                {
                    if (msg->type == ix::WebSocketMessageType::Open)
                    {
                        std::cout << "New connection" << std::endl;

                        // A connection state object is available, and has a default id
                        // You can subclass ConnectionState and pass an alternate factory
                        // to override it. It is useful if you want to store custom
                        // attributes per connection (authenticated bool flag, attributes, etc...)
                        std::cout << "id: " << connectionState->getId() << std::endl;

                        // The uri the client did connect to.
                        std::cout << "Uri: " << msg->openInfo.uri << std::endl;

                        std::cout << "Headers:" << std::endl;
                        for (auto it : msg->openInfo.headers)
                        {
                            std::cout << it.first << ": " << it.second << std::endl;
                        }
                    }
                    else if (msg->type == ix::WebSocketMessageType::Message)
                    {
                        // For an echo server, we just send back to the client whatever was received by the server
                        // All connected clients are available in an std::set. See the broadcast cpp example.
                        // Second parameter tells whether we are sending the message in binary or text mode.
                        // Here we send it in the same mode as it was received.
                        auto ws = webSocket.lock();
                        if (ws)
                        {
                            ws->send(msg->str, msg->binary);
                        }
                    }
                }
            }
        );
    }
);

auto res = server.listen();
if (!res.first)
{
    // Error handling
    return 1;
}

// Per message deflate connection is enabled by default. It can be disabled
// which might be helpful when running on low power devices such as a Rasbery Pi
server.disablePerMessageDeflate();

// Run the server in the background. Server can be stoped by calling server.stop()
server.start();

// Block until server.stop() is called.
server.wait();

```

### New api

The new API does not require to use 2 nested callbacks, which is a bit annoying. The real fix is that there was a memory leak due to a shared_ptr cycle, due to passing down a shared_ptr<WebSocket> down to the callbacks.

The webSocket reference is guaranteed to be always valid ; by design the callback will never be invoked with a null webSocket object.

```cpp
#include <ixwebsocket/IXWebSocketServer.h>

...

// Run a server on localhost at a given port.
// Bound host name, max connections and listen backlog can also be passed in as parameters.
int port = 8008;
std::string host("127.0.0.1"); // If you need this server to be accessible on a different machine, use "0.0.0.0"
ix::WebSocketServer server(port, host);

server.setOnClientMessageCallback([](std::shared_ptr<ix::ConnectionState> connectionState, ix::WebSocket & webSocket, const ix::WebSocketMessagePtr & msg) {
    // The ConnectionState object contains information about the connection,
    // at this point only the client ip address and the port.
    std::cout << "Remote ip: " << connectionState->getRemoteIp() << std::endl;

    if (msg->type == ix::WebSocketMessageType::Open)
    {
        std::cout << "New connection" << std::endl;

        // A connection state object is available, and has a default id
        // You can subclass ConnectionState and pass an alternate factory
        // to override it. It is useful if you want to store custom
        // attributes per connection (authenticated bool flag, attributes, etc...)
        std::cout << "id: " << connectionState->getId() << std::endl;

        // The uri the client did connect to.
        std::cout << "Uri: " << msg->openInfo.uri << std::endl;

        std::cout << "Headers:" << std::endl;
        for (auto it : msg->openInfo.headers)
        {
            std::cout << "\t" << it.first << ": " << it.second << std::endl;
        }
    }
    else if (msg->type == ix::WebSocketMessageType::Message)
    {
        // For an echo server, we just send back to the client whatever was received by the server
        // All connected clients are available in an std::set. See the broadcast cpp example.
        // Second parameter tells whether we are sending the message in binary or text mode.
        // Here we send it in the same mode as it was received.
        std::cout << "Received: " << msg->str << std::endl;

        webSocket.send(msg->str, msg->binary);
    }
});

auto res = server.listen();
if (!res.first)
{
    // Error handling
    return 1;
}

// Per message deflate connection is enabled by default. It can be disabled
// which might be helpful when running on low power devices such as a Rasbery Pi
server.disablePerMessageDeflate();

// Run the server in the background. Server can be stoped by calling server.stop()
server.start();

// Block until server.stop() is called.
server.wait();

```

### Heartbeat

You can configure an optional heartbeat / keep-alive for the WebSocket server. The heartbeat interval can be adjusted or disabled when constructing the `WebSocketServer`. Setting the interval to `-1` disables the heartbeat feature; this is the default setting. The parameter you set will be applied to every `WebSocket` object that the server creates.

To enable a 45 second heartbeat on a `WebSocketServer`:

```cpp
int pingIntervalSeconds = 45;
ix::WebSocketServer server(port, host, backlog, maxConnections, handshakeTimeoutSecs, addressFamily, pingIntervalSeconds);
```

## HTTP client API

```cpp
#include <ixwebsocket/IXHttpClient.h>

...

//
// Preparation
//
HttpClient httpClient;
HttpRequestArgsPtr args = httpClient.createRequest();

// Custom headers can be set
WebSocketHttpHeaders headers;
headers["Foo"] = "bar";
args->extraHeaders = headers;

// Timeout options
args->connectTimeout = connectTimeout;
args->transferTimeout = transferTimeout;

// Redirect options
args->followRedirects = followRedirects;
args->maxRedirects = maxRedirects;

// Misc
args->compress = compress; // Enable gzip compression
args->verbose = verbose;
args->logger = [](const std::string& msg)
{
    std::cout << msg;
};

//
// Synchronous Request
//
HttpResponsePtr out;
std::string url = "https://www.google.com";

// HEAD request
out = httpClient.head(url, args);

// GET request
out = httpClient.get(url, args);

// POST request with parameters
HttpParameters httpParameters;
httpParameters["foo"] = "bar";

// HTTP form data can be passed in as well, for multi-part upload of files
HttpFormDataParameters httpFormDataParameters;
httpParameters["baz"] = "booz";

out = httpClient.post(url, httpParameters, httpFormDataParameters, args);

// POST request with a body
out = httpClient.post(url, std::string("foo=bar"), args);

// PUT and PATCH are available too.

//
// Result
//
auto statusCode = response->statusCode; // Can be HttpErrorCode::Ok, HttpErrorCode::UrlMalformed, etc...
auto errorCode = response->errorCode; // 200, 404, etc...
auto responseHeaders = response->headers; // All the headers in a special case-insensitive unordered_map of (string, string)
auto body = response->body; // All the bytes from the response as an std::string
auto errorMsg = response->errorMsg; // Descriptive error message in case of failure
auto uploadSize = response->uploadSize; // Byte count of uploaded data
auto downloadSize = response->downloadSize; // Byte count of downloaded data

//
// Asynchronous Request
//
bool async = true;
HttpClient httpClient(async);
auto args = httpClient.createRequest(url, HttpClient::kGet);

// If you define a chunk callback it will be called repeteadly with the
// incoming data. This allows to process data on the go or write it to disk
// instead of accumulating the data in memory.
args.onChunkCallback = [](const std::string& data)
{
    // process data
};

// Push the request to a queue,
bool ok = httpClient.performRequest(args, [](const HttpResponsePtr& response)
    {
        // This callback execute in a background thread. Make sure you uses appropriate protection such as mutex
        auto statusCode = response->statusCode; // acess results

        // response->body is empty if onChunkCallback was used
    }
);

// ok will be false if your httpClient is not async

// A request in progress can be cancelled by setting the cancel flag. It does nothing if the request already completed.
args->cancel = true;
```

See this [issue](https://github.com/machinezone/IXWebSocket/issues/209) for links about uploading files with HTTP multipart.

## HTTP server API

```cpp
#include <ixwebsocket/IXHttpServer.h>

ix::HttpServer server(port, hostname);

auto res = server.listen();
if (!res.first)
{
    std::cerr << res.second << std::endl;
    return 1;
}

server.start();
server.wait();
```

If you want to handle how requests are processed, implement the setOnConnectionCallback callback, which takes an HttpRequestPtr as input, and returns an HttpResponsePtr. You can look at HttpServer::setDefaultConnectionCallback for a slightly more advanced callback example.

```cpp
setOnConnectionCallback(
    [this](HttpRequestPtr request,
           std::shared_ptr<ConnectionState> connectionState) -> HttpResponsePtr
    {
        // Build a string for the response
        std::stringstream ss;
        ss << connectionState->getRemoteIp();
           << " "
           << request->method
           << " "
           << request->uri;

        std::string content = ss.str();

        return std::make_shared<HttpResponse>(200, "OK",
                                              HttpErrorCode::Ok,
                                              WebSocketHttpHeaders(),
                                              content);
}
```

## TLS support and configuration

To leverage TLS features, the library must be compiled with the option `USE_TLS=1`.

If you are using OpenSSL, try to be on a version higher than 1.1.x as there there are thread safety problems with 1.0.x.

Then, secure sockets are automatically used when connecting to a `wss://*` url.

Additional TLS options can be configured by passing a `ix::SocketTLSOptions` instance to the
`setTLSOptions` on `ix::WebSocket` (or `ix::WebSocketServer` or `ix::HttpServer`)

```cpp
webSocket.setTLSOptions({
    .certFile = "path/to/cert/file.pem",
    .keyFile = "path/to/key/file.pem",
    .caFile = "path/to/trust/bundle/file.pem", // as a file, or in memory buffer in PEM format
    .tls = true // required in server mode
});
```

Specifying `certFile` and `keyFile` configures the certificate that will be used to communicate with TLS peers.

On a client, this is only necessary for connecting to servers that require a client certificate.

On a server, this is necessary for TLS support.

Specifying `caFile` configures the trusted roots bundle file (in PEM format) that will be used to verify peer certificates.
 - The special value of `SYSTEM` (the default) indicates that the system-configured trust bundle should be used; this is generally what you want when connecting to any publicly exposed API/server.
 - The special value of `NONE` can be used to disable peer verification; this is only recommended to rule out certificate verification when testing connectivity.
 - If the value contain the special value `-----BEGIN CERTIFICATE-----`, the value will be read from memory, and not from a file. This is convenient on platforms like Android where reading / writing to the file system can be challenging without proper permissions, or without knowing the location of a temp directory.

For a client, specifying `caFile` can be used if connecting to a server that uses a self-signed cert, or when using a custom CA in an internal environment.

For a server, specifying `caFile` implies that:
1. You require clients to present a certificate
1. It must be signed by one of the trusted roots in the file

By default, a destination's hostname is always validated against the certificate that it presents. To accept certificates with any hostname, set `ix::SocketTLSOptions::disable_hostname_validation` to `true`.
