/*
 *  httpd.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2020 Machine Zone, Inc. All rights reserved.
 *
 *  Buid with make httpd
 */

#include <ixwebsocket/IXHttpServer.h>
#include <sstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <port> <host>" << std::endl;
        std::cerr << " " << argv[0] << " 9090 127.0.0.1" << std::endl;
        std::cerr << " " << argv[0] << " 9090 0.0.0.0" << std::endl;
        return 1;
    }

    int port;
    std::stringstream ss;
    ss << argv[1];
    ss >> port;
    std::string hostname(argv[2]);

    std::cout << "Listening on " << hostname
              << ":" << port << std::endl;

    ix::HttpServer server(port, hostname);

    auto res = server.listen();
    if (!res.first)
    {
        std::cout << res.second << std::endl;
        return 1;
    }

    server.start();
    server.wait();

    return 0;
}
