#/bin/sh

grep VERSION ixwebsocket/IXWebSocketVersion.h | awk '{print $3}' | tr -d \"
