/*
 *  ws.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone, Inc. All rights reserved.
 */

//
// Main driver for websocket utilities
//

#include "linenoise.hpp"
#include <CLI11.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <ixwebsocket/IXBench.h>
#include <ixwebsocket/IXDNSLookup.h>
#include <ixwebsocket/IXGzipCodec.h>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSetThreadName.h>
#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXUserAgent.h>
#include <ixwebsocket/IXUuid.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketHttpHeaders.h>
#include <ixwebsocket/IXWebSocketProxyServer.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <msgpack11.hpp>
#include <mutex>
#include <queue>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#else
#include <process.h>
#define getpid _getpid
#endif

// for convenience
using msgpack11::MsgPack;

namespace
{
    std::pair<bool, std::vector<uint8_t>> load(const std::string& path)
    {
        std::vector<uint8_t> memblock;
        std::ifstream file(path);

        if (!file.is_open()) return std::make_pair(false, memblock);

        file.seekg(0, file.end);
        std::streamoff size = file.tellg();
        file.seekg(0, file.beg);

        memblock.reserve((size_t) size);
        memblock.insert(
            memblock.begin(), std::istream_iterator<char>(file), std::istream_iterator<char>());

        return std::make_pair(true, memblock);
    }

    std::pair<bool, std::string> readAsString(const std::string& path)
    {
        auto res = load(path);
        auto vec = res.second;
        return std::make_pair(res.first, std::string(vec.begin(), vec.end()));
    }

    std::string truncate(const std::string& str, size_t n)
    {
        if (str.size() < n)
        {
            return str;
        }
        else
        {
            return str.substr(0, n) + "...";
        }
    }

    std::string extractFilename(const std::string& path)
    {
        std::string::size_type idx;

        idx = path.rfind('/');
        if (idx != std::string::npos)
        {
            std::string filename = path.substr(idx + 1);
            return filename;
        }
        else
        {
            return path;
        }
    }

    std::string removeExtension(const std::string& path)
    {
        std::string::size_type idx;

        idx = path.rfind('.');
        if (idx != std::string::npos)
        {
            std::string filename = path.substr(0, idx);
            return filename;
        }
        else
        {
            return path;
        }
    }

    uint64_t djb2Hash(const std::vector<uint8_t>& data)
    {
        uint64_t hashAddress = 5381;

        for (auto&& c : data)
        {
            hashAddress = ((hashAddress << 5) + hashAddress) + c;
        }

        return hashAddress;
    }

    uint64_t djb2HashStr(const std::string& data)
    {
        uint64_t hashAddress = 5381;

        for (size_t i = 0; i < data.size(); ++i)
        {
            hashAddress = ((hashAddress << 5) + hashAddress) + data[i];
        }

        return hashAddress;
    }
} // namespace

namespace ix
{
    //
    // Autobahn test suite
    //
    // 1. First you need to generate a config file in a config folder,
    //    which can use a white list of test to execute (with globbing),
    //    or a black list of tests to ignore
    //
    // config/fuzzingserver.json
    // {
    //     "url": "ws://127.0.0.1:9001",
    //     "outdir": "./reports/clients",
    //     "cases": ["2.*"],
    //     "exclude-cases": [
    //     ],
    //     "exclude-agent-cases": {}
    // }
    //
    //
    // 2 Run the test server (using docker)
    // docker run -it --rm -v "${PWD}/config:/config" -v "${PWD}/reports:/reports" -p 9001:9001
    // --name fuzzingserver crossbario/autobahn-testsuite
    //
    // 3. Run this command
    //    ws autobahn -q --url ws://localhost:9001
    //
    // 4. A HTML report will be generated, you can inspect it to see if you are compliant or not
    //

    class AutobahnTestCase
    {
    public:
        AutobahnTestCase(const std::string& _url, bool quiet);
        void run();

    private:
        void log(const std::string& msg);

        std::string _url;
        ix::WebSocket _webSocket;

        bool _quiet;

        std::mutex _mutex;
        std::condition_variable _condition;
    };

    AutobahnTestCase::AutobahnTestCase(const std::string& url, bool quiet)
        : _url(url)
        , _quiet(quiet)
    {
        _webSocket.disableAutomaticReconnection();

        // FIXME: this should be on by default
        ix::WebSocketPerMessageDeflateOptions webSocketPerMessageDeflateOptions(
            true, false, false, 15, 15);
        _webSocket.setPerMessageDeflateOptions(webSocketPerMessageDeflateOptions);
    }

    void AutobahnTestCase::log(const std::string& msg)
    {
        if (!_quiet)
        {
            spdlog::info(msg);
        }
    }

    void AutobahnTestCase::run()
    {
        _webSocket.setUrl(_url);

        std::stringstream ss;
        log(std::string("Connecting to url: ") + _url);

        _webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            std::stringstream ss;
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                log("autobahn: connected");
                ss << "Uri: " << msg->openInfo.uri << std::endl;
                ss << "Handshake Headers:" << std::endl;
                for (auto it : msg->openInfo.headers)
                {
                    ss << it.first << ": " << it.second << std::endl;
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                ss << "autobahn: connection closed:";
                ss << " code " << msg->closeInfo.code;
                ss << " reason " << msg->closeInfo.reason << std::endl;

                _condition.notify_one();
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                ss << "Received " << msg->wireSize << " bytes" << std::endl;

                ss << "autobahn: received message: " << truncate(msg->str, 40) << std::endl;

                _webSocket.send(msg->str, msg->binary);
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;

                // And error can happen, in which case the test-case is marked done
                _condition.notify_one();
            }
            else if (msg->type == ix::WebSocketMessageType::Fragment)
            {
                ss << "Received message fragment" << std::endl;
            }
            else if (msg->type == ix::WebSocketMessageType::Ping)
            {
                ss << "Received ping" << std::endl;
            }
            else if (msg->type == ix::WebSocketMessageType::Pong)
            {
                ss << "Received pong" << std::endl;
            }
            else
            {
                ss << "Invalid ix::WebSocketMessageType" << std::endl;
            }

            log(ss.str());
        });

        _webSocket.start();

        log("Waiting for test completion ...");
        std::unique_lock<std::mutex> lock(_mutex);
        _condition.wait(lock);

        _webSocket.stop();
    }

    bool generateReport(const std::string& url)
    {
        ix::WebSocket webSocket;
        std::string reportUrl(url);
        reportUrl += "/updateReports?agent=ixwebsocket";
        webSocket.setUrl(reportUrl);
        webSocket.disableAutomaticReconnection();

        std::atomic<bool> success(true);
        std::condition_variable condition;

        webSocket.setOnMessageCallback([&condition, &success](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Close)
            {
                spdlog::info("Report generated");
                condition.notify_one();
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                std::stringstream ss;
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                spdlog::info(ss.str());

                success = false;
            }
        });

        webSocket.start();
        std::mutex mutex;
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock);
        webSocket.stop();

        if (!success)
        {
            spdlog::error("Cannot generate report at url {}", reportUrl);
        }

        return success;
    }

    int getTestCaseCount(const std::string& url)
    {
        ix::WebSocket webSocket;
        std::string caseCountUrl(url);
        caseCountUrl += "/getCaseCount";
        webSocket.setUrl(caseCountUrl);
        webSocket.disableAutomaticReconnection();

        int count = -1;
        std::condition_variable condition;

        webSocket.setOnMessageCallback([&condition, &count](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Close)
            {
                condition.notify_one();
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                std::stringstream ss;
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                spdlog::info(ss.str());

                condition.notify_one();
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                // response is a string
                std::stringstream ss;
                ss << msg->str;
                ss >> count;
            }
        });

        webSocket.start();
        std::mutex mutex;
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock);
        webSocket.stop();

        if (count == -1)
        {
            spdlog::error("Cannot retrieve test case count at url {}", caseCountUrl);
        }

        return count;
    }

    //
    // make && bench ws autobahn --url ws://localhost:9001
    //
    int ws_autobahn_main(const std::string& url, bool quiet)
    {
        int testCasesCount = getTestCaseCount(url);
        spdlog::info("Test cases count: {}", testCasesCount);

        if (testCasesCount == -1)
        {
            spdlog::error("Cannot retrieve test case count at url {}", url);
            return 1;
        }

        testCasesCount++;

        for (int i = 1; i < testCasesCount; ++i)
        {
            spdlog::info("Execute test case {}", i);

            int caseNumber = i;

            std::stringstream ss;
            ss << url << "/runCase?case=" << caseNumber << "&agent=ixwebsocket";

            std::string url(ss.str());

            AutobahnTestCase testCase(url, quiet);
            testCase.run();
        }

        return generateReport(url) ? 0 : 1;
    }

    /*
     *  ws_chat.cpp
     *  Author: Benjamin Sergeant
     *  Copyright (c) 2017-2019 Machine Zone, Inc. All rights reserved.
     */

    //
    // Simple chat program that talks to a broadcast server
    // Broadcast server can be ran with `ws broadcast_server`
    //

    class WebSocketChat
    {
    public:
        WebSocketChat(const std::string& url, const std::string& user);

        void subscribe(const std::string& channel);
        void start();
        void stop();
        bool isReady() const;

        void sendMessage(const std::string& text);
        size_t getReceivedMessagesCount() const;

        std::string encodeMessage(const std::string& text);
        std::pair<std::string, std::string> decodeMessage(const std::string& str);

    private:
        std::string _url;
        std::string _user;
        ix::WebSocket _webSocket;
        std::queue<std::string> _receivedQueue;

        void log(const std::string& msg);
    };

    WebSocketChat::WebSocketChat(const std::string& url, const std::string& user)
        : _url(url)
        , _user(user)
    {
        ;
    }

    void WebSocketChat::log(const std::string& msg)
    {
        spdlog::info(msg);
    }

    size_t WebSocketChat::getReceivedMessagesCount() const
    {
        return _receivedQueue.size();
    }

    bool WebSocketChat::isReady() const
    {
        return _webSocket.getReadyState() == ix::ReadyState::Open;
    }

    void WebSocketChat::stop()
    {
        _webSocket.stop();
    }

    void WebSocketChat::start()
    {
        _webSocket.setUrl(_url);

        std::stringstream ss;
        log(std::string("Connecting to url: ") + _url);

        _webSocket.setOnMessageCallback([this](const WebSocketMessagePtr& msg) {
            std::stringstream ss;
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                log("ws chat: connected");
                spdlog::info("Uri: {}", msg->openInfo.uri);
                spdlog::info("Headers:");
                for (auto it : msg->openInfo.headers)
                {
                    spdlog::info("{}: {}", it.first, it.second);
                }

                spdlog::info("ws chat: user {} connected !", _user);
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                ss << "ws chat user disconnected: " << _user;
                ss << " code " << msg->closeInfo.code;
                ss << " reason " << msg->closeInfo.reason << std::endl;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                auto result = decodeMessage(msg->str);

                // Our "chat" / "broacast" node.js server does not send us
                // the messages we send, so we don't have to filter it out.

                // store text
                _receivedQueue.push(result.second);

                ss << std::endl
                   << result.first << "(" << msg->wireSize << " bytes)"
                   << " > " << result.second << std::endl
                   << _user << " > ";
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                log(ss.str());
            }
            else
            {
                ss << "Invalid ix::WebSocketMessageType";
                log(ss.str());
            }
        });

        _webSocket.start();
    }

    std::pair<std::string, std::string> WebSocketChat::decodeMessage(const std::string& str)
    {
        std::string errMsg;
        MsgPack msg = MsgPack::parse(str, errMsg);

        std::string msg_user = msg["user"].string_value();
        std::string msg_text = msg["text"].string_value();

        return std::pair<std::string, std::string>(msg_user, msg_text);
    }

    std::string WebSocketChat::encodeMessage(const std::string& text)
    {
        std::map<MsgPack, MsgPack> obj;
        obj["user"] = _user;
        obj["text"] = text;

        MsgPack msg(obj);

        std::string output = msg.dump();
        return output;
    }

    void WebSocketChat::sendMessage(const std::string& text)
    {
        _webSocket.sendBinary(encodeMessage(text));
    }

    int ws_chat_main(const std::string& url, const std::string& user)
    {
        spdlog::info("Type Ctrl-D to exit prompt...");
        WebSocketChat webSocketChat(url, user);
        webSocketChat.start();

        while (true)
        {
            // Read line
            std::string line;
            std::cout << user << " > " << std::flush;
            std::getline(std::cin, line);

            if (!std::cin)
            {
                break;
            }

            webSocketChat.sendMessage(line);
        }

        spdlog::info("");
        webSocketChat.stop();

        return 0;
    }

    class WebSocketConnect
    {
    public:
        WebSocketConnect(const std::string& _url,
                         const std::string& headers,
                         bool disableAutomaticReconnection,
                         bool disablePerMessageDeflate,
                         bool binaryMode,
                         uint32_t maxWaitBetweenReconnectionRetries,
                         const ix::SocketTLSOptions& tlsOptions,
                         const std::string& subprotocol,
                         int pingIntervalSecs,
                         bool decompressGzipMessages);

        void subscribe(const std::string& channel);
        void start();
        void stop();

        int getSentBytes()
        {
            return _sentBytes;
        }
        int getReceivedBytes()
        {
            return _receivedBytes;
        }

        void sendMessage(const std::string& text);

    private:
        std::string _url;
        WebSocketHttpHeaders _headers;
        ix::WebSocket _webSocket;
        bool _disablePerMessageDeflate;
        bool _binaryMode;
        std::atomic<int> _receivedBytes;
        std::atomic<int> _sentBytes;
        bool _decompressGzipMessages;

        void log(const std::string& msg);
        WebSocketHttpHeaders parseHeaders(const std::string& data);
    };

    WebSocketConnect::WebSocketConnect(const std::string& url,
                                       const std::string& headers,
                                       bool disableAutomaticReconnection,
                                       bool disablePerMessageDeflate,
                                       bool binaryMode,
                                       uint32_t maxWaitBetweenReconnectionRetries,
                                       const ix::SocketTLSOptions& tlsOptions,
                                       const std::string& subprotocol,
                                       int pingIntervalSecs,
                                       bool decompressGzipMessages)
        : _url(url)
        , _disablePerMessageDeflate(disablePerMessageDeflate)
        , _binaryMode(binaryMode)
        , _receivedBytes(0)
        , _sentBytes(0)
        , _decompressGzipMessages(decompressGzipMessages)
    {
        if (disableAutomaticReconnection)
        {
            _webSocket.disableAutomaticReconnection();
        }
        _webSocket.setMaxWaitBetweenReconnectionRetries(maxWaitBetweenReconnectionRetries);
        _webSocket.setTLSOptions(tlsOptions);
        _webSocket.setPingInterval(pingIntervalSecs);

        _headers = parseHeaders(headers);

        if (!subprotocol.empty())
        {
            _webSocket.addSubProtocol(subprotocol);
        }

        WebSocket::setTrafficTrackerCallback([this](int size, bool incoming) {
            if (incoming)
            {
                _receivedBytes += size;
            }
            else
            {
                _sentBytes += size;
            }
        });
    }

    void WebSocketConnect::log(const std::string& msg)
    {
        std::cout << msg << std::endl;
    }

    WebSocketHttpHeaders WebSocketConnect::parseHeaders(const std::string& data)
    {
        WebSocketHttpHeaders headers;

        // Split by \n
        std::string token;
        std::stringstream tokenStream(data);

        while (std::getline(tokenStream, token))
        {
            std::size_t pos = token.rfind(':');

            // Bail out if last '.' is found
            if (pos == std::string::npos) continue;

            auto key = token.substr(0, pos);
            auto val = token.substr(pos + 1);

            spdlog::info("{}: {}", key, val);
            headers[key] = val;
        }

        return headers;
    }

    void WebSocketConnect::stop()
    {
        {
            Bench bench("ws_connect: stop connection");
            _webSocket.stop();
        }
    }

    void WebSocketConnect::start()
    {
        _webSocket.setUrl(_url);
        _webSocket.setExtraHeaders(_headers);

        if (_disablePerMessageDeflate)
        {
            _webSocket.disablePerMessageDeflate();
        }
        else
        {
            ix::WebSocketPerMessageDeflateOptions webSocketPerMessageDeflateOptions(
                true, false, false, 15, 15);
            _webSocket.setPerMessageDeflateOptions(webSocketPerMessageDeflateOptions);
        }

        std::stringstream ss;
        log(std::string("Connecting to url: ") + _url);

        _webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            std::stringstream ss;
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                spdlog::info("ws_connect: connected");
                spdlog::info("Uri: {}", msg->openInfo.uri);
                spdlog::info("Headers:");
                for (auto it : msg->openInfo.headers)
                {
                    spdlog::info("{}: {}", it.first, it.second);
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                ss << "ws_connect: connection closed:";
                ss << " code " << msg->closeInfo.code;
                ss << " reason " << msg->closeInfo.reason << std::endl;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                spdlog::info("Received {} bytes", msg->wireSize);

                std::string payload = msg->str;
                if (_decompressGzipMessages)
                {
                    std::string decompressedBytes;
                    if (gzipDecompress(payload, decompressedBytes))
                    {
                        payload = decompressedBytes;
                    }
                    else
                    {
                        spdlog::error("Error decompressing: {}", payload);
                    }
                }

                ss << "ws_connect: received message: " << payload;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Fragment)
            {
                spdlog::info("Received message fragment");
            }
            else if (msg->type == ix::WebSocketMessageType::Ping)
            {
                spdlog::info("Received ping");
            }
            else if (msg->type == ix::WebSocketMessageType::Pong)
            {
                spdlog::info("Received pong {}", msg->str);
            }
            else
            {
                ss << "Invalid ix::WebSocketMessageType";
                log(ss.str());
            }
        });

        _webSocket.start();
    }

    void WebSocketConnect::sendMessage(const std::string& text)
    {
        if (_binaryMode)
        {
            _webSocket.sendBinary(text);
        }
        else
        {
            _webSocket.sendText(text);
        }
    }

    int ws_connect_main(const std::string& url,
                        const std::string& headers,
                        bool disableAutomaticReconnection,
                        bool disablePerMessageDeflate,
                        bool binaryMode,
                        uint32_t maxWaitBetweenReconnectionRetries,
                        const ix::SocketTLSOptions& tlsOptions,
                        const std::string& subprotocol,
                        int pingIntervalSecs,
                        bool decompressGzipMessages)
    {
        std::cout << "Type Ctrl-D to exit prompt..." << std::endl;
        WebSocketConnect webSocketChat(url,
                                       headers,
                                       disableAutomaticReconnection,
                                       disablePerMessageDeflate,
                                       binaryMode,
                                       maxWaitBetweenReconnectionRetries,
                                       tlsOptions,
                                       subprotocol,
                                       pingIntervalSecs,
                                       decompressGzipMessages);
        webSocketChat.start();

        while (true)
        {
            // Read line
            std::string line;
            auto quit = linenoise::Readline("> ", line);

            if (quit)
            {
                break;
            }

            if (line == "/stop")
            {
                spdlog::info("Stopping connection...");
                webSocketChat.stop();
                continue;
            }

            if (line == "/start")
            {
                spdlog::info("Starting connection...");
                webSocketChat.start();
                continue;
            }

            webSocketChat.sendMessage(line);

            // Add text to history
            linenoise::AddHistory(line.c_str());
        }

        spdlog::info("");
        webSocketChat.stop();

        spdlog::info("Received {} bytes", webSocketChat.getReceivedBytes());
        spdlog::info("Sent {} bytes", webSocketChat.getSentBytes());

        return 0;
    }

    int ws_dns_lookup(const std::string& hostname)
    {
        auto dnsLookup = std::make_shared<DNSLookup>(hostname, 80);

        std::string errMsg;

        auto res = dnsLookup->resolve(errMsg, [] { return false; });

        auto addr = res->ai_addr;

        // FIXME: this display weird addresses / we could steal libuv inet.c
        // code which display correct results

        char str[INET_ADDRSTRLEN];
        ix::inet_ntop(AF_INET, &addr, str, INET_ADDRSTRLEN);

        spdlog::info("host: {} ip: {}", hostname, str);

        return 0;
    }

    int ws_gzip(const std::string& filename, int runCount)
    {
        auto res = readAsString(filename);
        bool found = res.first;
        if (!found)
        {
            spdlog::error("Cannot read content of {}", filename);
            return 1;
        }

        spdlog::info("gzip input: {} size {} cksum {}",
                     filename,
                     res.second.size(),
                     djb2HashStr(res.second));

        std::string compressedBytes;

        spdlog::info("compressing {} times", runCount);
        std::vector<uint64_t> durations;
        {
            Bench bench("compressing file");
            bench.setReported();

            for (int i = 0; i < runCount; ++i)
            {
                bench.reset();
                compressedBytes = gzipCompress(res.second);
                bench.record();
                durations.push_back(bench.getDuration());
            }

            size_t medianIdx = durations.size() / 2;
            uint64_t medianRuntime = durations[medianIdx];
            spdlog::info("median runtime to compress file: {}", medianRuntime);
        }

        std::string outputFilename(filename);
        outputFilename += ".gz";

        std::ofstream f;
        f.open(outputFilename);
        f << compressedBytes;
        f.close();

        spdlog::info("gzip output: {} size {} cksum {}",
                     outputFilename,
                     compressedBytes.size(),
                     djb2HashStr(compressedBytes));

        return 0;
    }

    int ws_gunzip(const std::string& filename)
    {
        spdlog::info("filename to gunzip: {}", filename);

        auto res = readAsString(filename);
        bool found = res.first;
        if (!found)
        {
            spdlog::error("Cannot read content of {}", filename);
            return 1;
        }

        spdlog::info("gunzip input: {} size {} cksum {}",
                     filename,
                     res.second.size(),
                     djb2HashStr(res.second));

        std::string decompressedBytes;

        {
            Bench bench("decompressing file");
            if (!gzipDecompress(res.second, decompressedBytes))
            {
                spdlog::error("Cannot decompress content of {}", filename);
                return 1;
            }
        }

        std::string outputFilename(removeExtension(filename));

        std::ofstream f;
        f.open(outputFilename);
        if (!f.is_open())
        {
            spdlog::error("Cannot open {} for writing", outputFilename);
            return 1;
        }
        f << decompressedBytes;
        f.close();

        spdlog::info("gunzip output: {} size {} cksum {}",
                     outputFilename,
                     decompressedBytes.size(),
                     djb2HashStr(decompressedBytes));

        return 0;
    }

    int ws_autoroute(const std::string& url,
                     bool disablePerMessageDeflate,
                     const ix::SocketTLSOptions& tlsOptions,
                     const std::string& subprotocol,
                     int pingIntervalSecs,
                     int msgCount)
    {
        Bench bench("ws_autoroute full test");

        // Our websocket object
        ix::WebSocket webSocket;

        std::string fullUrl(url);
        fullUrl += "/";
        fullUrl += std::to_string(msgCount);

        webSocket.setUrl(fullUrl);
        webSocket.setTLSOptions(tlsOptions);
        webSocket.setPingInterval(pingIntervalSecs);
        webSocket.disableAutomaticReconnection();

        if (disablePerMessageDeflate)
        {
            webSocket.disablePerMessageDeflate();
        }

        if (!subprotocol.empty())
        {
            webSocket.addSubProtocol(subprotocol);
        }

        std::atomic<uint64_t> receivedCountPerSecs(0);
        std::atomic<uint64_t> target(msgCount);
        std::mutex conditionVariableMutex;
        std::condition_variable condition;

        std::atomic<bool> stop(false);
        std::chrono::time_point<std::chrono::high_resolution_clock> start;

        // Setup a callback to be fired
        // when a message or an event (open, close, ping, pong, error) is received
        webSocket.setOnMessageCallback(
            [&receivedCountPerSecs, &target, &stop, &condition, &bench, &start](
                const ix::WebSocketMessagePtr& msg) {
                if (msg->type == ix::WebSocketMessageType::Message)
                {
                    receivedCountPerSecs++;

                    target -= 1;
                    if (target == 0)
                    {
                        stop = true;
                        condition.notify_one();

                        bench.report();

                        auto now = std::chrono::high_resolution_clock::now();
                        auto milliseconds =
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
                        auto duration = milliseconds.count();

                        spdlog::info("AUTOROUTE IXWebSocket :: {} ms", duration);
                    }
                }
                else if (msg->type == ix::WebSocketMessageType::Open)
                {
                    bench.reset();

                    spdlog::info("ws_autoroute: connected");
                    spdlog::info("Uri: {}", msg->openInfo.uri);
                    spdlog::info("Headers:");
                    for (auto it : msg->openInfo.headers)
                    {
                        spdlog::info("{}: {}", it.first, it.second);
                    }

                    start = std::chrono::high_resolution_clock::now();
                }
                else if (msg->type == ix::WebSocketMessageType::Pong)
                {
                    spdlog::info("Received pong {}", msg->str);
                }
                else if (msg->type == ix::WebSocketMessageType::Close)
                {
                    spdlog::info("ws_autoroute: connection closed");
                }
            });

        auto timer = [&receivedCountPerSecs, &stop] {
            setThreadName("Timer");
            while (!stop)
            {
                //
                // We cannot write to sentCount and receivedCount
                // as those are used externally, so we need to introduce
                // our own counters
                //
                std::stringstream ss;
                ss << "messages received per second: " << receivedCountPerSecs;

                spdlog::info(ss.str());

                receivedCountPerSecs = 0;

                auto duration = std::chrono::seconds(1);
                std::this_thread::sleep_for(duration);
            }
        };

        std::thread t1(timer);

        // Now that our callback is setup, we can start our background thread and receive messages
        std::cout << "Connecting to " << url << "..." << std::endl;
        webSocket.start();

        // Wait for all the messages to be received
        std::unique_lock<std::mutex> lock(conditionVariableMutex);
        condition.wait(lock);

        t1.join();
        webSocket.stop();

        return 0;
    }

    int ws_echo_server_main(int port,
                            bool greetings,
                            const std::string& hostname,
                            const ix::SocketTLSOptions& tlsOptions,
                            bool ipv6,
                            bool disablePerMessageDeflate,
                            bool disablePong,
                            const std::string& httpHeaderAuthorization)
    {
        spdlog::info("Listening on {}:{}", hostname, port);

        ix::WebSocketServer server(port,
                                   hostname,
                                   SocketServer::kDefaultTcpBacklog,
                                   SocketServer::kDefaultMaxConnections,
                                   WebSocketServer::kDefaultHandShakeTimeoutSecs,
                                   (ipv6) ? AF_INET6 : AF_INET);

        server.setTLSOptions(tlsOptions);

        if (disablePerMessageDeflate)
        {
            spdlog::info("Disable per message deflate");
            server.disablePerMessageDeflate();
        }

        if (disablePong)
        {
            spdlog::info("Disable responding to PING messages with PONG");
            server.disablePong();
        }

        server.setOnClientMessageCallback(
            [greetings, httpHeaderAuthorization](std::shared_ptr<ConnectionState> connectionState,
                                                 WebSocket& webSocket,
                                                 const WebSocketMessagePtr& msg) {
                auto remoteIp = connectionState->getRemoteIp();
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    spdlog::info("New connection");
                    spdlog::info("remote ip: {}", remoteIp);
                    spdlog::info("id: {}", connectionState->getId());
                    spdlog::info("Uri: {}", msg->openInfo.uri);
                    spdlog::info("Headers:");
                    for (auto it : msg->openInfo.headers)
                    {
                        spdlog::info("{}: {}", it.first, it.second);
                    }

                    if (!httpHeaderAuthorization.empty())
                    {
                        auto authorization = msg->openInfo.headers["Authorization"];
                        if (authorization != httpHeaderAuthorization)
                        {
                            webSocket.close(4001, "Permission denied");
                        }
                        else
                        {
                            webSocket.sendText("Authorization suceeded!");
                        }
                    }

                    if (greetings)
                    {
                        webSocket.sendText("Welcome !");
                    }
                }
                else if (msg->type == ix::WebSocketMessageType::Close)
                {
                    spdlog::info("Closed connection: client id {} code {} reason {}",
                                 connectionState->getId(),
                                 msg->closeInfo.code,
                                 msg->closeInfo.reason);
                }
                else if (msg->type == ix::WebSocketMessageType::Error)
                {
                    spdlog::error("Connection error: {}", msg->errorInfo.reason);
                    spdlog::error("#retries: {}", msg->errorInfo.retries);
                    spdlog::error("Wait time(ms): {}", msg->errorInfo.wait_time);
                    spdlog::error("HTTP Status: {}", msg->errorInfo.http_status);
                }
                else if (msg->type == ix::WebSocketMessageType::Message)
                {
                    spdlog::info("Received {} bytes", msg->wireSize);
                    webSocket.send(msg->str, msg->binary);
                }
            });

        auto res = server.listen();
        if (!res.first)
        {
            spdlog::error(res.second);
            return 1;
        }

        server.start();
        server.wait();

        return 0;
    }

    WebSocketHttpHeaders parseHeaders(const std::string& data)
    {
        WebSocketHttpHeaders headers;

        // Split by \n
        std::string token;
        std::stringstream tokenStream(data);

        while (std::getline(tokenStream, token))
        {
            std::size_t pos = token.rfind(':');

            // Bail out if last '.' is found
            if (pos == std::string::npos) continue;

            auto key = token.substr(0, pos);
            auto val = token.substr(pos + 1);

            spdlog::info("{}: {}", key, val);
            headers[key] = val;
        }

        return headers;
    }

    //
    // Useful endpoint to test HTTP post
    // https://postman-echo.com/post
    //
    HttpParameters parseHttpParameters(const std::string& data)
    {
        HttpParameters httpParameters;

        // Split by \n
        std::string token;
        std::stringstream tokenStream(data);

        while (std::getline(tokenStream, token))
        {
            std::size_t pos = token.rfind('=');

            // Bail out if last '.' is found
            if (pos == std::string::npos) continue;

            auto key = token.substr(0, pos);
            auto val = token.substr(pos + 1);

            spdlog::info("{}: {}", key, val);
            httpParameters[key] = val;
        }

        return httpParameters;
    }

    HttpFormDataParameters parseHttpFormDataParameters(const std::string& data)
    {
        HttpFormDataParameters httpFormDataParameters;

        // Split by \n
        std::string token;
        std::stringstream tokenStream(data);

        while (std::getline(tokenStream, token))
        {
            std::size_t pos = token.rfind('=');

            // Bail out if last '.' is found
            if (pos == std::string::npos) continue;

            auto key = token.substr(0, pos);
            auto val = token.substr(pos + 1);

            spdlog::info("{}: {}", key, val);

            if (val[0] == '@')
            {
                std::string filename = token.substr(pos + 2);

                auto res = readAsString(filename);
                bool found = res.first;
                if (!found)
                {
                    spdlog::error("Cannot read content of {}", filename);
                    continue;
                }

                val = res.second;
            }

            httpFormDataParameters[key] = val;
        }

        return httpFormDataParameters;
    }

    int ws_http_client_main(const std::string& url,
                            const std::string& headersData,
                            const std::string& data,
                            const std::string& formData,
                            const std::string& dataBinary,
                            bool headersOnly,
                            int connectTimeout,
                            int transferTimeout,
                            bool followRedirects,
                            int maxRedirects,
                            bool verbose,
                            bool save,
                            const std::string& output,
                            bool compress,
                            bool compressRequest,
                            const ix::SocketTLSOptions& tlsOptions)
    {
        HttpClient httpClient;
        httpClient.setTLSOptions(tlsOptions);

        auto args = httpClient.createRequest();
        args->extraHeaders = parseHeaders(headersData);
        args->connectTimeout = connectTimeout;
        args->transferTimeout = transferTimeout;
        args->followRedirects = followRedirects;
        args->maxRedirects = maxRedirects;
        args->verbose = verbose;
        args->compress = compress;
        args->compressRequest = compressRequest;
        args->logger = [](const std::string& msg) { spdlog::info(msg); };
        args->onProgressCallback = [verbose](int current, int total) -> bool {
            if (verbose)
            {
                spdlog::info("Downloaded {} bytes out of {}", current, total);
            }
            return true;
        };

        HttpParameters httpParameters = parseHttpParameters(data);
        HttpFormDataParameters httpFormDataParameters = parseHttpFormDataParameters(formData);

        HttpResponsePtr response;
        if (headersOnly)
        {
            response = httpClient.head(url, args);
        }
        else if (data.empty() && formData.empty() && dataBinary.empty())
        {
            response = httpClient.get(url, args);
        }
        else if (!dataBinary.empty())
        {
            std::string body = dataBinary;
            if (compressRequest)
            {
                body = gzipCompress(dataBinary);
            }
            response = httpClient.request(url, "POST", body, args, 0);
        }
        else
        {
            response = httpClient.post(url, httpParameters, httpFormDataParameters, args);
        }

        spdlog::info("");

        for (auto it : response->headers)
        {
            spdlog::info("{}: {}", it.first, it.second);
        }

        spdlog::info("Upload size: {}", response->uploadSize);
        spdlog::info("Download size: {}", response->downloadSize);

        spdlog::info("Status: {}", response->statusCode);
        if (response->errorCode != HttpErrorCode::Ok)
        {
            spdlog::error("error message: {}", response->errorMsg);
        }

        if (!headersOnly && response->errorCode == HttpErrorCode::Ok)
        {
            if (save || !output.empty())
            {
                // FIMXE we should decode the url first
                std::string filename = extractFilename(url);
                if (!output.empty())
                {
                    filename = output;
                }

                if (filename.empty())
                {
                    spdlog::error("Cannot save content to disk: No output file supplied, and not "
                                  "filename could be extracted from the url {}",
                                  url);
                }
                else
                {
                    spdlog::info("Writing to disk: {}", filename);
                    std::ofstream out(filename);
                    out << response->body;
                }
            }
            else
            {
                if (response->headers["Content-Type"] != "application/octet-stream")
                {
                    spdlog::info("body: {}", response->body);
                }
                else
                {
                    spdlog::info("Binary output can mess up your terminal.");
                    spdlog::info("Use the -O flag to save the file to disk.");
                    spdlog::info("You can also use the --output option to specify a filename.");
                }
            }
        }

        return 0;
    }

    int ws_httpd_main(int port,
                      const std::string& hostname,
                      bool redirect,
                      const std::string& redirectUrl,
                      bool debug,
                      const ix::SocketTLSOptions& tlsOptions)
    {
        spdlog::info("Listening on {}:{}", hostname, port);

        ix::HttpServer server(port, hostname);
        server.setTLSOptions(tlsOptions);

        if (redirect)
        {
            server.makeRedirectServer(redirectUrl);
        }

        if (debug)
        {
            server.makeDebugServer();
        }

        auto res = server.listen();
        if (!res.first)
        {
            spdlog::error(res.second);
            return 1;
        }

        server.start();
        server.wait();

        return 0;
    }

    class WebSocketPingPong
    {
    public:
        WebSocketPingPong(const std::string& _url, const ix::SocketTLSOptions& tlsOptions);

        void subscribe(const std::string& channel);
        void start();
        void stop();

        void ping(const std::string& text);
        void send(const std::string& text);

    private:
        std::string _url;
        ix::WebSocket _webSocket;

        void log(const std::string& msg);
    };

    WebSocketPingPong::WebSocketPingPong(const std::string& url,
                                         const ix::SocketTLSOptions& tlsOptions)
        : _url(url)
    {
        _webSocket.setTLSOptions(tlsOptions);
    }

    void WebSocketPingPong::log(const std::string& msg)
    {
        spdlog::info(msg);
    }

    void WebSocketPingPong::stop()
    {
        _webSocket.stop();
    }

    void WebSocketPingPong::start()
    {
        _webSocket.setUrl(_url);

        std::stringstream ss;
        log(std::string("Connecting to url: ") + _url);

        _webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            spdlog::info("Received {} bytes", msg->wireSize);

            std::stringstream ss;
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                log("ping_pong: connected");

                spdlog::info("Uri: {}", msg->openInfo.uri);
                spdlog::info("Headers:");
                for (auto it : msg->openInfo.headers)
                {
                    spdlog::info("{}: {}", it.first, it.second);
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                ss << "ping_pong: disconnected:"
                   << " code " << msg->closeInfo.code << " reason " << msg->closeInfo.reason
                   << msg->str;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                ss << "ping_pong: received message: " << msg->str;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Ping)
            {
                ss << "ping_pong: received ping message: " << msg->str;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Pong)
            {
                ss << "ping_pong: received pong message: " << msg->str;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                log(ss.str());
            }
            else
            {
                ss << "Invalid ix::WebSocketMessageType";
                log(ss.str());
            }
        });

        _webSocket.start();
    }

    void WebSocketPingPong::ping(const std::string& text)
    {
        if (!_webSocket.ping(text).success)
        {
            std::cerr << "Failed to send ping message. Message too long (> 125 bytes) or endpoint "
                         "is disconnected"
                      << std::endl;
        }
    }

    void WebSocketPingPong::send(const std::string& text)
    {
        _webSocket.send(text);
    }

    int ws_ping_pong_main(const std::string& url, const ix::SocketTLSOptions& tlsOptions)
    {
        spdlog::info("Type Ctrl-D to exit prompt...");
        WebSocketPingPong webSocketPingPong(url, tlsOptions);
        webSocketPingPong.start();

        while (true)
        {
            std::string text;
            std::cout << "> " << std::flush;
            std::getline(std::cin, text);

            if (!std::cin)
            {
                break;
            }

            if (text == "/close")
            {
                webSocketPingPong.send(text);
            }
            else
            {
                webSocketPingPong.ping(text);
            }
        }

        std::cout << std::endl;
        webSocketPingPong.stop();

        return 0;
    }

    int ws_push_server(int port,
                       const std::string& hostname,
                       const ix::SocketTLSOptions& tlsOptions,
                       bool ipv6,
                       bool disablePerMessageDeflate,
                       bool disablePong,
                       const std::string& sendMsg)
    {
        spdlog::info("Listening on {}:{}", hostname, port);

        ix::WebSocketServer server(port,
                                   hostname,
                                   SocketServer::kDefaultTcpBacklog,
                                   SocketServer::kDefaultMaxConnections,
                                   WebSocketServer::kDefaultHandShakeTimeoutSecs,
                                   (ipv6) ? AF_INET6 : AF_INET);

        server.setTLSOptions(tlsOptions);

        if (disablePerMessageDeflate)
        {
            spdlog::info("Disable per message deflate");
            server.disablePerMessageDeflate();
        }

        if (disablePong)
        {
            spdlog::info("Disable responding to PING messages with PONG");
            server.disablePong();
        }

        // push one million messages
        std::atomic<bool> stop(false);

        server.setOnClientMessageCallback(
            [&sendMsg, &stop](std::shared_ptr<ConnectionState> connectionState,
                              WebSocket& webSocket,
                              const WebSocketMessagePtr& msg) {
                auto remoteIp = connectionState->getRemoteIp();
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    spdlog::info("New connection");
                    spdlog::info("remote ip: {}", remoteIp);
                    spdlog::info("id: {}", connectionState->getId());
                    spdlog::info("Uri: {}", msg->openInfo.uri);
                    spdlog::info("Headers:");
                    for (auto it : msg->openInfo.headers)
                    {
                        spdlog::info("{}: {}", it.first, it.second);
                    }

                    // Parse the msg count from the uri.
                    int msgCount = -1;
                    std::stringstream ss;
                    auto uriSize = msg->openInfo.uri.size();
                    ss << msg->openInfo.uri.substr(1, uriSize - 1);
                    ss >> msgCount;

                    if (msgCount == -1)
                    {
                        spdlog::info("Error parsing message count, closing connection");
                        webSocket.close();
                    }
                    else
                    {
                        bool binary = false;
                        for (int i = 0; i < msgCount; ++i)
                        {
                            auto sendInfo = webSocket.send(sendMsg, binary);
                            if (!sendInfo.success)
                            {
                                spdlog::info("Error sending message, closing connection");
                                webSocket.close();
                                break;
                            }
                        }
                    }
                }
                else if (msg->type == ix::WebSocketMessageType::Close)
                {
                    spdlog::info("Closed connection: client id {} code {} reason {}",
                                 connectionState->getId(),
                                 msg->closeInfo.code,
                                 msg->closeInfo.reason);
                    stop = true;
                }
                else if (msg->type == ix::WebSocketMessageType::Error)
                {
                    spdlog::error("Connection error: {}", msg->errorInfo.reason);
                    spdlog::error("#retries: {}", msg->errorInfo.retries);
                    spdlog::error("Wait time(ms): {}", msg->errorInfo.wait_time);
                    spdlog::error("HTTP Status: {}", msg->errorInfo.http_status);
                }
                else if (msg->type == ix::WebSocketMessageType::Message)
                {
                    spdlog::info("Received {} bytes", msg->wireSize);
                    webSocket.send(msg->str, msg->binary);
                }
            });

        auto res = server.listen();
        if (!res.first)
        {
            spdlog::error(res.second);
            return 1;
        }

        server.start();

        while (!stop)
        {
            auto duration = std::chrono::seconds(1);
            std::this_thread::sleep_for(duration);
        }

        server.stop();

        return 0;
    }

    class WebSocketReceiver
    {
    public:
        WebSocketReceiver(const std::string& _url,
                          bool enablePerMessageDeflate,
                          int delayMs,
                          const ix::SocketTLSOptions& tlsOptions);

        void subscribe(const std::string& channel);
        void start();
        void stop();

        void waitForConnection();
        void waitForMessage();
        void handleMessage(const std::string& str);

    private:
        std::string _url;
        std::string _id;
        ix::WebSocket _webSocket;
        bool _enablePerMessageDeflate;
        int _delayMs;
        int _receivedFragmentCounter;

        std::mutex _conditionVariableMutex;
        std::condition_variable _condition;

        std::string extractFilename(const std::string& path);
        void handleError(const std::string& errMsg, const std::string& id);
        void log(const std::string& msg);
    };

    WebSocketReceiver::WebSocketReceiver(const std::string& url,
                                         bool enablePerMessageDeflate,
                                         int delayMs,
                                         const ix::SocketTLSOptions& tlsOptions)
        : _url(url)
        , _enablePerMessageDeflate(enablePerMessageDeflate)
        , _delayMs(delayMs)
        , _receivedFragmentCounter(0)
    {
        _webSocket.disableAutomaticReconnection();
        _webSocket.setTLSOptions(tlsOptions);
    }

    void WebSocketReceiver::stop()
    {
        _webSocket.stop();
    }

    void WebSocketReceiver::log(const std::string& msg)
    {
        spdlog::info(msg);
    }

    void WebSocketReceiver::waitForConnection()
    {
        spdlog::info("{}: Connecting...", "ws_receive");

        std::unique_lock<std::mutex> lock(_conditionVariableMutex);
        _condition.wait(lock);
    }

    void WebSocketReceiver::waitForMessage()
    {
        spdlog::info("{}: Waiting for message...", "ws_receive");

        std::unique_lock<std::mutex> lock(_conditionVariableMutex);
        _condition.wait(lock);
    }

    // We should cleanup the file name and full path further to remove .. as well
    std::string WebSocketReceiver::extractFilename(const std::string& path)
    {
        std::string::size_type idx;

        idx = path.rfind('/');
        if (idx != std::string::npos)
        {
            std::string filename = path.substr(idx + 1);
            return filename;
        }
        else
        {
            return path;
        }
    }

    void WebSocketReceiver::handleError(const std::string& errMsg, const std::string& id)
    {
        std::map<MsgPack, MsgPack> pdu;
        pdu["kind"] = "error";
        pdu["id"] = id;
        pdu["message"] = errMsg;

        MsgPack msg(pdu);
        _webSocket.sendBinary(msg.dump());
    }

    void WebSocketReceiver::handleMessage(const std::string& str)
    {
        spdlog::info("ws_receive: Received message: {}", str.size());

        std::string errMsg;
        MsgPack data = MsgPack::parse(str, errMsg);
        if (!errMsg.empty())
        {
            handleError("ws_receive: Invalid MsgPack", std::string());
            return;
        }

        spdlog::info("id: {}", data["id"].string_value());

        std::vector<uint8_t> content = data["content"].binary_items();
        spdlog::info("ws_receive: Content size: {}", content.size());

        // Validate checksum
        uint64_t cksum = djb2Hash(content);
        auto cksumRef = data["djb2_hash"].string_value();

        spdlog::info("ws_receive: Computed hash: {}", cksum);
        spdlog::info("ws_receive: Reference hash: {}", cksumRef);

        if (std::to_string(cksum) != cksumRef)
        {
            handleError("Hash mismatch.", std::string());
            return;
        }

        std::string filename = data["filename"].string_value();
        filename = extractFilename(filename);

        std::string filenameTmp = filename + ".tmp";

        spdlog::info("ws_receive: Writing to disk: {}", filenameTmp);
        std::ofstream out(filenameTmp);
        std::string contentAsString(content.begin(), content.end());
        out << contentAsString;
        out.close();

        spdlog::info("ws_receive: Renaming {} to {}", filenameTmp, filename);
        rename(filenameTmp.c_str(), filename.c_str());

        std::map<MsgPack, MsgPack> pdu;
        pdu["ack"] = true;
        pdu["id"] = data["id"];
        pdu["filename"] = data["filename"];

        spdlog::info("Sending ack to sender");
        MsgPack msg(pdu);
        _webSocket.sendBinary(msg.dump());
    }

    void WebSocketReceiver::start()
    {
        _webSocket.setUrl(_url);
        ix::WebSocketPerMessageDeflateOptions webSocketPerMessageDeflateOptions(
            _enablePerMessageDeflate, false, false, 15, 15);
        _webSocket.setPerMessageDeflateOptions(webSocketPerMessageDeflateOptions);

        std::stringstream ss;
        log(std::string("ws_receive: Connecting to url: ") + _url);

        _webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            std::stringstream ss;
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                _condition.notify_one();

                log("ws_receive: connected");
                spdlog::info("Uri: {}", msg->openInfo.uri);
                spdlog::info("Headers:");
                for (auto it : msg->openInfo.headers)
                {
                    spdlog::info("{}: {}", it.first, it.second);
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                ss << "ws_receive: connection closed:";
                ss << " code " << msg->closeInfo.code;
                ss << " reason " << msg->closeInfo.reason << std::endl;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                ss << "ws_receive: transfered " << msg->wireSize << " bytes";
                log(ss.str());
                handleMessage(msg->str);
                _condition.notify_one();
            }
            else if (msg->type == ix::WebSocketMessageType::Fragment)
            {
                ss << "ws_receive: received fragment " << _receivedFragmentCounter++;
                log(ss.str());

                if (_delayMs > 0)
                {
                    // Introduce an arbitrary delay, to simulate a slow connection
                    std::chrono::duration<double, std::milli> duration(_delayMs);
                    std::this_thread::sleep_for(duration);
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                ss << "ws_receive ";
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Ping)
            {
                log("ws_receive: received ping");
            }
            else if (msg->type == ix::WebSocketMessageType::Pong)
            {
                log("ws_receive: received pong");
            }
            else
            {
                ss << "ws_receive: Invalid ix::WebSocketMessageType";
                log(ss.str());
            }
        });

        _webSocket.start();
    }

    void wsReceive(const std::string& url,
                   bool enablePerMessageDeflate,
                   int delayMs,
                   const ix::SocketTLSOptions& tlsOptions)
    {
        WebSocketReceiver webSocketReceiver(url, enablePerMessageDeflate, delayMs, tlsOptions);
        webSocketReceiver.start();

        webSocketReceiver.waitForConnection();

        webSocketReceiver.waitForMessage();

        std::chrono::duration<double, std::milli> duration(1000);
        std::this_thread::sleep_for(duration);

        spdlog::info("ws_receive: Done !");
        webSocketReceiver.stop();
    }

    int ws_receive_main(const std::string& url,
                        bool enablePerMessageDeflate,
                        int delayMs,
                        const ix::SocketTLSOptions& tlsOptions)
    {
        wsReceive(url, enablePerMessageDeflate, delayMs, tlsOptions);
        return 0;
    }

    class WebSocketSender
    {
    public:
        WebSocketSender(const std::string& _url,
                        bool enablePerMessageDeflate,
                        const ix::SocketTLSOptions& tlsOptions);

        void subscribe(const std::string& channel);
        void start();
        void stop();

        void waitForConnection();
        void waitForAck();

        bool sendMessage(const std::string& filename, bool throttle);

    private:
        std::string _url;
        std::string _id;
        ix::WebSocket _webSocket;
        bool _enablePerMessageDeflate;

        std::atomic<bool> _connected;

        std::mutex _conditionVariableMutex;
        std::condition_variable _condition;

        void log(const std::string& msg);
    };

    WebSocketSender::WebSocketSender(const std::string& url,
                                     bool enablePerMessageDeflate,
                                     const ix::SocketTLSOptions& tlsOptions)
        : _url(url)
        , _enablePerMessageDeflate(enablePerMessageDeflate)
        , _connected(false)
    {
        _webSocket.disableAutomaticReconnection();
        _webSocket.setTLSOptions(tlsOptions);
    }

    void WebSocketSender::stop()
    {
        _webSocket.stop();
    }

    void WebSocketSender::log(const std::string& msg)
    {
        spdlog::info(msg);
    }

    void WebSocketSender::waitForConnection()
    {
        spdlog::info("{}: Connecting...", "ws_send");

        std::unique_lock<std::mutex> lock(_conditionVariableMutex);
        _condition.wait(lock);
    }

    void WebSocketSender::waitForAck()
    {
        spdlog::info("{}: Waiting for ack...", "ws_send");

        std::unique_lock<std::mutex> lock(_conditionVariableMutex);
        _condition.wait(lock);
    }

    void WebSocketSender::start()
    {
        _webSocket.setUrl(_url);

        ix::WebSocketPerMessageDeflateOptions webSocketPerMessageDeflateOptions(
            _enablePerMessageDeflate, false, false, 15, 15);
        _webSocket.setPerMessageDeflateOptions(webSocketPerMessageDeflateOptions);

        std::stringstream ss;
        log(std::string("ws_send: Connecting to url: ") + _url);

        _webSocket.setOnMessageCallback([this](const WebSocketMessagePtr& msg) {
            std::stringstream ss;
            if (msg->type == ix::WebSocketMessageType::Open)
            {
                _connected = true;

                _condition.notify_one();

                log("ws_send: connected");
                spdlog::info("Uri: {}", msg->openInfo.uri);
                spdlog::info("Headers:");
                for (auto it : msg->openInfo.headers)
                {
                    spdlog::info("{}: {}", it.first, it.second);
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
            {
                _connected = false;

                ss << "ws_send: connection closed:";
                ss << " code " << msg->closeInfo.code;
                ss << " reason " << msg->closeInfo.reason << std::endl;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                _condition.notify_one();

                ss << "ws_send: received message (" << msg->wireSize << " bytes)";
                log(ss.str());

                std::string errMsg;
                MsgPack data = MsgPack::parse(msg->str, errMsg);
                if (!errMsg.empty())
                {
                    spdlog::info("Invalid MsgPack response");
                    return;
                }

                std::string id = data["id"].string_value();
                if (_id != id)
                {
                    spdlog::info("Invalid id");
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Error)
            {
                ss << "ws_send ";
                ss << "Connection error: " << msg->errorInfo.reason << std::endl;
                ss << "#retries: " << msg->errorInfo.retries << std::endl;
                ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                log(ss.str());
            }
            else if (msg->type == ix::WebSocketMessageType::Ping)
            {
                spdlog::info("ws_send: received ping");
            }
            else if (msg->type == ix::WebSocketMessageType::Pong)
            {
                spdlog::info("ws_send: received pong");
            }
            else if (msg->type == ix::WebSocketMessageType::Fragment)
            {
                spdlog::info("ws_send: received fragment");
            }
            else
            {
                ss << "ws_send: Invalid ix::WebSocketMessageType";
                log(ss.str());
            }
        });

        _webSocket.start();
    }

    bool WebSocketSender::sendMessage(const std::string& filename, bool throttle)
    {
        std::vector<uint8_t> content;
        {
            Bench bench("ws_send: load file from disk");
            auto res = load(filename);
            content = res.second;
        }

        _id = uuid4();

        std::map<MsgPack, MsgPack> pdu;
        pdu["kind"] = "send";
        pdu["id"] = _id;
        pdu["content"] = content;
        auto hash = djb2Hash(content);
        pdu["djb2_hash"] = std::to_string(hash);
        pdu["filename"] = filename;

        MsgPack msg(pdu);

        auto serializedMsg = msg.dump();
        spdlog::info("ws_send: sending {} bytes", serializedMsg.size());

        Bench bench("ws_send: Sending file through websocket");
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

        if (!result.success)
        {
            spdlog::error("ws_send: Error sending file.");
            return false;
        }

        if (!_connected)
        {
            spdlog::error("ws_send: Got disconnected from the server");
            return false;
        }

        spdlog::info("ws_send: sent {} bytes", serializedMsg.size());

        do
        {
            size_t bufferedAmount = _webSocket.bufferedAmount();
            spdlog::info("ws_send: {} bytes left to be sent", bufferedAmount);

            std::chrono::duration<double, std::milli> duration(500);
            std::this_thread::sleep_for(duration);
        } while (_webSocket.bufferedAmount() != 0 && _connected);

        if (_connected)
        {
            bench.report();
            auto duration = bench.getDuration();
            auto transferRate = 1000 * content.size() / duration;
            transferRate /= (1024 * 1024);
            spdlog::info("ws_send: Send transfer rate: {} MB/s", transferRate);
        }
        else
        {
            spdlog::error("ws_send: Got disconnected from the server");
        }

        return _connected;
    }

    void wsSend(const std::string& url,
                const std::string& path,
                bool enablePerMessageDeflate,
                bool throttle,
                const ix::SocketTLSOptions& tlsOptions)
    {
        WebSocketSender webSocketSender(url, enablePerMessageDeflate, tlsOptions);
        webSocketSender.start();

        webSocketSender.waitForConnection();

        spdlog::info("ws_send: Sending...");
        if (webSocketSender.sendMessage(path, throttle))
        {
            webSocketSender.waitForAck();
            spdlog::info("ws_send: Done !");
        }
        else
        {
            spdlog::error("ws_send: Error sending file.");
        }

        webSocketSender.stop();
    }

    int ws_send_main(const std::string& url,
                     const std::string& path,
                     bool disablePerMessageDeflate,
                     const ix::SocketTLSOptions& tlsOptions)
    {
        bool throttle = false;
        bool enablePerMessageDeflate = !disablePerMessageDeflate;

        wsSend(url, path, enablePerMessageDeflate, throttle, tlsOptions);
        return 0;
    }

    int ws_transfer_main(int port,
                         const std::string& hostname,
                         const ix::SocketTLSOptions& tlsOptions)
    {
        spdlog::info("Listening on {}:{}", hostname, port);

        ix::WebSocketServer server(port, hostname);
        server.setTLSOptions(tlsOptions);

        server.setOnClientMessageCallback(
            [&server](std::shared_ptr<ConnectionState> connectionState,
                      WebSocket& webSocket,
                      const WebSocketMessagePtr& msg) {
                auto remoteIp = connectionState->getRemoteIp();
                if (msg->type == ix::WebSocketMessageType::Open)
                {
                    spdlog::info("ws_transfer: New connection");
                    spdlog::info("remote ip: {}", remoteIp);
                    spdlog::info("id: {}", connectionState->getId());
                    spdlog::info("Uri: {}", msg->openInfo.uri);
                    spdlog::info("Headers:");
                    for (auto it : msg->openInfo.headers)
                    {
                        spdlog::info("{}: {}", it.first, it.second);
                    }
                }
                else if (msg->type == ix::WebSocketMessageType::Close)
                {
                    spdlog::info("ws_transfer: Closed connection: client id {} code {} reason {}",
                                 connectionState->getId(),
                                 msg->closeInfo.code,
                                 msg->closeInfo.reason);
                    auto remaining = server.getClients().size() - 1;
                    spdlog::info("ws_transfer: {} remaining clients", remaining);
                }
                else if (msg->type == ix::WebSocketMessageType::Error)
                {
                    std::stringstream ss;
                    ss << "ws_transfer: Connection error: " << msg->errorInfo.reason << std::endl;
                    ss << "#retries: " << msg->errorInfo.retries << std::endl;
                    ss << "Wait time(ms): " << msg->errorInfo.wait_time << std::endl;
                    ss << "HTTP Status: " << msg->errorInfo.http_status << std::endl;
                    spdlog::info(ss.str());
                }
                else if (msg->type == ix::WebSocketMessageType::Fragment)
                {
                    spdlog::info("ws_transfer: Received message fragment ");
                }
                else if (msg->type == ix::WebSocketMessageType::Message)
                {
                    spdlog::info("ws_transfer: Received {} bytes", msg->wireSize);
                    size_t receivers = 0;
                    for (auto&& client : server.getClients())
                    {
                        if (client.get() != &webSocket)
                        {
                            auto readyState = client->getReadyState();
                            auto id = connectionState->getId();

                            if (readyState == ReadyState::Open)
                            {
                                ++receivers;
                                client->send(
                                    msg->str, msg->binary, [&id](int current, int total) -> bool {
                                        spdlog::info("{}: [client {}]: Step {} out of {}",
                                                     "ws_transfer",
                                                     id,
                                                     current,
                                                     total);
                                        return true;
                                    });
                                do
                                {
                                    size_t bufferedAmount = client->bufferedAmount();

                                    spdlog::info("{}: [client {}]: {} bytes left to send",
                                                 "ws_transfer",
                                                 id,
                                                 bufferedAmount);

                                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                                } while (client->bufferedAmount() != 0 &&
                                         client->getReadyState() == ReadyState::Open);
                            }
                            else
                            {
                                std::string readyStateString =
                                    readyState == ReadyState::Connecting ? "Connecting"
                                    : readyState == ReadyState::Closing  ? "Closing"
                                                                         : "Closed";
                                size_t bufferedAmount = client->bufferedAmount();

                                spdlog::info(
                                    "{}: [client {}]: has readystate {} bytes left to be sent {}",
                                    "ws_transfer",
                                    id,
                                    readyStateString,
                                    bufferedAmount);
                            }
                        }
                    }
                    if (!receivers)
                    {
                        spdlog::info("ws_transfer: no remaining receivers");
                    }
                }
            });

        auto res = server.listen();
        if (!res.first)
        {
            spdlog::info(res.second);
            return 1;
        }

        server.start();
        server.wait();

        return 0;
    }
} // namespace ix

int main(int argc, char** argv)
{
    ix::setThreadName("ws main thread");
    ix::initNetSystem();

    spdlog::set_level(spdlog::level::debug);

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // Display command.
    if (getenv("DEBUG"))
    {
        std::stringstream ss;
        ss << "Command: ";
        for (int i = 0; i < argc; ++i)
        {
            ss << argv[i] << " ";
        }
        spdlog::info(ss.str());
    }

    CLI::App app {"ws is a websocket tool"};

    std::string url("ws://127.0.0.1:8008");
    std::string path;
    std::string user;
    std::string data;
    std::string formData;
    std::string binaryData;
    std::string headers;
    std::string output;
    std::string hostname("127.0.0.1");
    std::string pidfile;
    std::string channel;
    std::string filter;
    std::string position;
    std::string message;
    std::string timer;
    std::string configPath;
    std::string subprotocol;
    std::string remoteHost;
    std::string logfile;
    std::string moduleName;
    std::string republishChannel;
    std::string publisherRolename;
    std::string publisherRolesecret;
    std::string sendMsg("hello world");
    std::string filename;
    std::string httpHeaderAuthorization;
    ix::SocketTLSOptions tlsOptions;
    std::string ciphers;
    std::string redirectUrl;
    bool headersOnly = false;
    bool followRedirects = false;
    bool verbose = false;
    bool save = false;
    bool quiet = false;
    bool compress = false;
    bool compressRequest = false;
    bool disableAutomaticReconnection = false;
    bool disablePerMessageDeflate = false;
    bool greetings = false;
    bool ipv6 = false;
    bool binaryMode = false;
    bool redirect = false;
    bool version = false;
    bool verifyNone = false;
    bool disablePong = false;
    bool debug = false;
    int port = 8008;
    int connectTimeOut = 60;
    int transferTimeout = 1800;
    int maxRedirects = 5;
    int delayMs = -1;
    int msgCount = 1000 * 1000;
    uint32_t maxWaitBetweenReconnectionRetries = 10 * 1000; // 10 seconds
    int pingIntervalSecs = 30;
    int runCount = 1;
    bool decompressGzipMessages = false;

    auto addGenericOptions = [&pidfile](CLI::App* app) {
        app->add_option("--pidfile", pidfile, "Pid file");
    };

    auto addTLSOptions = [&tlsOptions, &verifyNone](CLI::App* app) {
        app->add_option(
               "--cert-file", tlsOptions.certFile, "Path to the (PEM format) TLS cert file")
            ->check(CLI::ExistingPath);
        app->add_option("--key-file", tlsOptions.keyFile, "Path to the (PEM format) TLS key file")
            ->check(CLI::ExistingPath);
        app->add_option("--ca-file", tlsOptions.caFile, "Path to the (PEM format) ca roots file")
            ->check(CLI::ExistingPath);
        app->add_option("--ciphers",
                        tlsOptions.ciphers,
                        "A (comma/space/colon) separated list of ciphers to use for TLS");
        app->add_flag("--tls", tlsOptions.tls, "Enable TLS (server only)");
        app->add_flag("--verify_none", verifyNone, "Disable peer cert verification");
        app->add_flag("--disable-hostname-validation", tlsOptions.disable_hostname_validation, "Disable validation of certificates' hostnames");
    };

    app.add_flag("--version", version, "Print ws version");
    app.add_option("--logfile", logfile, "path where all logs will be redirected");

    CLI::App* sendApp = app.add_subcommand("send", "Send a file");
    sendApp->fallthrough();
    sendApp->add_option("url", url, "Connection url")->required();
    sendApp->add_option("path", path, "Path to the file to send")
        ->required()
        ->check(CLI::ExistingPath);
    sendApp->add_flag("-x", disablePerMessageDeflate, "Disable per message deflate");
    addGenericOptions(sendApp);
    addTLSOptions(sendApp);

    CLI::App* receiveApp = app.add_subcommand("receive", "Receive a file");
    receiveApp->fallthrough();
    receiveApp->add_option("url", url, "Connection url")->required();
    receiveApp->add_option("--delay",
                           delayMs,
                           "Delay (ms) to wait after receiving a fragment"
                           " to artificially slow down the receiver");
    receiveApp->add_option("--pidfile", pidfile, "Pid file");
    addTLSOptions(receiveApp);

    CLI::App* transferApp = app.add_subcommand("transfer", "Broadcasting server");
    transferApp->fallthrough();
    transferApp->add_option("--port", port, "Connection url");
    transferApp->add_option("--host", hostname, "Hostname");
    transferApp->add_option("--pidfile", pidfile, "Pid file");
    addTLSOptions(transferApp);

    CLI::App* connectApp = app.add_subcommand("connect", "Connect to a remote server");
    connectApp->fallthrough();
    connectApp->add_option("url", url, "Connection url")->required();
    connectApp->add_option("-H", headers, "Header")->join();
    connectApp->add_flag("-d", disableAutomaticReconnection, "Disable Automatic Reconnection");
    connectApp->add_flag("-x", disablePerMessageDeflate, "Disable per message deflate");
    connectApp->add_flag("-b", binaryMode, "Send in binary mode");
    connectApp->add_option("--max_wait",
                           maxWaitBetweenReconnectionRetries,
                           "Max Wait Time between reconnection retries");
    connectApp->add_option("--ping_interval", pingIntervalSecs, "Interval between sending pings");
    connectApp->add_option("--subprotocol", subprotocol, "Subprotocol");
    connectApp->add_flag("-g", decompressGzipMessages, "Decompress gziped messages");
    addGenericOptions(connectApp);
    addTLSOptions(connectApp);

    CLI::App* echoClientApp = app.add_subcommand("autoroute", "Test websocket client performance");
    echoClientApp->fallthrough();
    echoClientApp->add_option("url", url, "Connection url")->required();
    echoClientApp->add_flag("-x", disablePerMessageDeflate, "Disable per message deflate");
    echoClientApp->add_option(
        "--ping_interval", pingIntervalSecs, "Interval between sending pings");
    echoClientApp->add_option("--subprotocol", subprotocol, "Subprotocol");
    echoClientApp->add_option("--msg_count", msgCount, "Total message count to be sent");
    addTLSOptions(echoClientApp);

    CLI::App* chatApp = app.add_subcommand("chat", "Group chat");
    chatApp->fallthrough();
    chatApp->add_option("url", url, "Connection url")->required();
    chatApp->add_option("user", user, "User name")->required();

    CLI::App* echoServerApp = app.add_subcommand("echo_server", "Echo server");
    echoServerApp->fallthrough();
    echoServerApp->add_option("--port", port, "Port");
    echoServerApp->add_option("--host", hostname, "Hostname");
    echoServerApp->add_option("--http_authorization_header", httpHeaderAuthorization, "Hostname");
    echoServerApp->add_flag("-q", quiet, "Quiet / only display warnings and errors");
    echoServerApp->add_flag("-g", greetings, "Greet");
    echoServerApp->add_flag("-6", ipv6, "IpV6");
    echoServerApp->add_flag("-x", disablePerMessageDeflate, "Disable per message deflate");
    echoServerApp->add_flag("-p", disablePong, "Disable sending PONG in response to PING");
    addGenericOptions(echoServerApp);
    addTLSOptions(echoServerApp);

    CLI::App* pushServerApp = app.add_subcommand("push_server", "Push server");
    pushServerApp->fallthrough();
    pushServerApp->add_option("--port", port, "Port");
    pushServerApp->add_option("--host", hostname, "Hostname");
    pushServerApp->add_flag("-q", quiet, "Quiet / only display warnings and errors");
    pushServerApp->add_flag("-6", ipv6, "IpV6");
    pushServerApp->add_flag("-x", disablePerMessageDeflate, "Disable per message deflate");
    pushServerApp->add_flag("-p", disablePong, "Disable sending PONG in response to PING");
    pushServerApp->add_option("--send_msg", sendMsg, "Send message");
    addTLSOptions(pushServerApp);

    CLI::App* broadcastServerApp = app.add_subcommand("broadcast_server", "Broadcasting server");
    broadcastServerApp->fallthrough();
    broadcastServerApp->add_option("--port", port, "Port");
    broadcastServerApp->add_option("--host", hostname, "Hostname");
    addTLSOptions(broadcastServerApp);

    CLI::App* pingPongApp = app.add_subcommand("ping", "Ping pong");
    pingPongApp->fallthrough();
    pingPongApp->add_option("url", url, "Connection url")->required();
    addTLSOptions(pingPongApp);

    CLI::App* httpClientApp = app.add_subcommand("curl", "HTTP Client");
    httpClientApp->fallthrough();
    httpClientApp->add_option("url", url, "Connection url")->required();
    httpClientApp->add_option("-d", data, "Form data")->join();
    httpClientApp->add_option("-F", formData, "Form data")->join();
    httpClientApp->add_option("--data-binary", binaryData, "Body binary data")->join();
    httpClientApp->add_option("-H", headers, "Header")->join();
    httpClientApp->add_option("--output", output, "Output file");
    httpClientApp->add_flag("-I", headersOnly, "Send a HEAD request");
    httpClientApp->add_flag("-L", followRedirects, "Follow redirects");
    httpClientApp->add_option("--max-redirects", maxRedirects, "Max Redirects");
    httpClientApp->add_flag("-v", verbose, "Verbose");
    httpClientApp->add_flag("-O", save, "Save output to disk");
    httpClientApp->add_flag("--compressed", compress, "Enable gzip compression");
    httpClientApp->add_flag("--compress_request", compressRequest, "Compress request with gzip");
    httpClientApp->add_option("--connect-timeout", connectTimeOut, "Connection timeout");
    httpClientApp->add_option("--transfer-timeout", transferTimeout, "Transfer timeout");
    addTLSOptions(httpClientApp);

    CLI::App* httpServerApp = app.add_subcommand("httpd", "HTTP server");
    httpServerApp->fallthrough();
    httpServerApp->add_option("--port", port, "Port");
    httpServerApp->add_option("--host", hostname, "Hostname");
    httpServerApp->add_flag("-L", redirect, "Redirect all request to redirect_url");
    httpServerApp->add_option("--redirect_url", redirectUrl, "Url to redirect to");
    httpServerApp->add_flag("-D", debug, "Debug server");
    addTLSOptions(httpServerApp);

    CLI::App* autobahnApp = app.add_subcommand("autobahn", "Test client Autobahn compliance");
    autobahnApp->fallthrough();
    autobahnApp->add_option("--url", url, "url");
    autobahnApp->add_flag("-q", quiet, "Quiet");

    CLI::App* proxyServerApp = app.add_subcommand("proxy_server", "Proxy server");
    proxyServerApp->fallthrough();
    proxyServerApp->add_option("--port", port, "Port");
    proxyServerApp->add_option("--host", hostname, "Hostname");
    proxyServerApp->add_option("--remote_host", remoteHost, "Remote Hostname");
    proxyServerApp->add_flag("-v", verbose, "Verbose");
    proxyServerApp->add_option("--config_path", configPath, "Path to config data")
        ->check(CLI::ExistingPath);
    addGenericOptions(proxyServerApp);
    addTLSOptions(proxyServerApp);

    CLI::App* dnsLookupApp = app.add_subcommand("dnslookup", "DNS lookup");
    dnsLookupApp->fallthrough();
    dnsLookupApp->add_option("host", hostname, "Hostname")->required();

    CLI::App* gzipApp = app.add_subcommand("gzip", "Gzip compressor");
    gzipApp->fallthrough();
    gzipApp->add_option("filename", filename, "Filename")->required();
    gzipApp->add_option("--run_count", runCount, "Number of time to run the compression");

    CLI::App* gunzipApp = app.add_subcommand("gunzip", "Gzip decompressor");
    gunzipApp->fallthrough();
    gunzipApp->add_option("filename", filename, "Filename")->required();

    CLI11_PARSE(app, argc, argv);

    // pid file handling
    if (!pidfile.empty())
    {
        unlink(pidfile.c_str());

        std::ofstream f;
        f.open(pidfile);
        f << getpid();
        f.close();
    }

    if (verifyNone)
    {
        tlsOptions.caFile = "NONE";
    }

    if (tlsOptions.isUsingSystemDefaults())
    {
#if defined(__APPLE__)
#if defined(IXWEBSOCKET_USE_MBED_TLS) || defined(IXWEBSOCKET_USE_OPEN_SSL)
        // We could try to load some system certs as well, but this is easy enough
        tlsOptions.caFile = "/etc/ssl/cert.pem";
#endif
#elif defined(__linux__)
#if defined(IXWEBSOCKET_USE_MBED_TLS)
        std::vector<std::string> caFiles = {
            "/etc/ssl/certs/ca-bundle.crt",       // CentOS
            "/etc/ssl/certs/ca-certificates.crt", // Alpine
        };

        for (auto&& caFile : caFiles)
        {
            if (fileExists(caFile))
            {
                tlsOptions.caFile = caFile;
                break;
            }
        }
#endif
#endif
    }

    if (!logfile.empty())
    {
        try
        {
            auto fileLogger = spdlog::basic_logger_mt("ws", logfile);
            spdlog::set_default_logger(fileLogger);
            spdlog::flush_every(std::chrono::seconds(1));

            std::cerr << "All logs will be redirected to " << logfile << std::endl;
        }
        catch (const spdlog::spdlog_ex& ex)
        {
            std::cerr << "Fatal error, log init failed: " << ex.what() << std::endl;
            ix::uninitNetSystem();
            return 1;
        }
    }

    if (quiet)
    {
        spdlog::set_level(spdlog::level::info);
    }

    int ret = 1;
    if (app.got_subcommand("connect"))
    {
        ret = ix::ws_connect_main(url,
                                  headers,
                                  disableAutomaticReconnection,
                                  disablePerMessageDeflate,
                                  binaryMode,
                                  maxWaitBetweenReconnectionRetries,
                                  tlsOptions,
                                  subprotocol,
                                  pingIntervalSecs,
                                  decompressGzipMessages);
    }
    else if (app.got_subcommand("autoroute"))
    {
        ret = ix::ws_autoroute(
            url, disablePerMessageDeflate, tlsOptions, subprotocol, pingIntervalSecs, msgCount);
    }
    else if (app.got_subcommand("echo_server"))
    {
        ret = ix::ws_echo_server_main(port,
                                      greetings,
                                      hostname,
                                      tlsOptions,
                                      ipv6,
                                      disablePerMessageDeflate,
                                      disablePong,
                                      httpHeaderAuthorization);
    }
    else if (app.got_subcommand("push_server"))
    {
        ret = ix::ws_push_server(
            port, hostname, tlsOptions, ipv6, disablePerMessageDeflate, disablePong, sendMsg);
    }
    else if (app.got_subcommand("transfer") || app.got_subcommand("broadcast_server"))
    {
        ix::WebSocketServer server(port, hostname);
        server.setTLSOptions(tlsOptions);
        server.makeBroadcastServer();
        if (!server.listenAndStart())
        {
            spdlog::error("Error while starting the server");
        }
        else
        {
            server.wait();
        }
    }
    else if (app.got_subcommand("send"))
    {
        ret = ix::ws_send_main(url, path, disablePerMessageDeflate, tlsOptions);
    }
    else if (app.got_subcommand("receive"))
    {
        bool enablePerMessageDeflate = false;
        ret = ix::ws_receive_main(url, enablePerMessageDeflate, delayMs, tlsOptions);
    }
    else if (app.got_subcommand("chat"))
    {
        ret = ix::ws_chat_main(url, user);
    }
    else if (app.got_subcommand("ping"))
    {
        ret = ix::ws_ping_pong_main(url, tlsOptions);
    }
    else if (app.got_subcommand("curl"))
    {
        ret = ix::ws_http_client_main(url,
                                      headers,
                                      data,
                                      formData,
                                      binaryData,
                                      headersOnly,
                                      connectTimeOut,
                                      transferTimeout,
                                      followRedirects,
                                      maxRedirects,
                                      verbose,
                                      save,
                                      output,
                                      compress,
                                      compressRequest,
                                      tlsOptions);
    }
    else if (app.got_subcommand("httpd"))
    {
        ret = ix::ws_httpd_main(port, hostname, redirect, redirectUrl, debug, tlsOptions);
    }
    else if (app.got_subcommand("autobahn"))
    {
        ret = ix::ws_autobahn_main(url, quiet);
    }
    else if (app.got_subcommand("proxy_server"))
    {
        ix::RemoteUrlsMapping remoteUrlsMapping;

        if (!configPath.empty())
        {
            auto res = readAsString(configPath);
            bool found = res.first;
            if (!found)
            {
                spdlog::error("Cannot read config file {} from disk", configPath);
            }
            else
            {
                // Split by \n
                std::string token;
                std::stringstream tokenStream(res.second);

                while (std::getline(tokenStream, token))
                {
                    std::size_t pos = token.rfind(':');

                    // Bail out if last '.' is found
                    if (pos == std::string::npos) continue;

                    auto key = token.substr(0, pos);
                    auto val = token.substr(pos + 1);

                    spdlog::info("{}: {}", key, val);

                    remoteUrlsMapping[key] = val;
                }
            }
        }

        ret = ix::websocket_proxy_server_main(
            port, hostname, tlsOptions, remoteHost, remoteUrlsMapping, verbose);
    }
    else if (app.got_subcommand("dnslookup"))
    {
        ret = ix::ws_dns_lookup(hostname);
    }
    else if (app.got_subcommand("gzip"))
    {
        ret = ix::ws_gzip(filename, runCount);
    }
    else if (app.got_subcommand("gunzip"))
    {
        ret = ix::ws_gunzip(filename);
    }
    else if (version)
    {
        std::cout << "ws " << ix::userAgent() << std::endl;
    }
    else
    {
        spdlog::error("A subcommand or --version is required");
    }

    ix::uninitNetSystem();
    return ret;
}
