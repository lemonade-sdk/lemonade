
## WebSocket Client performance

We will run a client and a server on the same machine, connecting to localhost. This bench is run on a MacBook Pro from 2015. We can receive over 200,000 (small) messages per second, another way to put it is that it takes 5 micro-second to receive and process one message. This is an indication about the minimal latency to receive messages.

### Receiving messages

By using the push_server ws sub-command, the server will send the same message in a loop to any connected client.

```
ws push_server -q --send_msg 'yo'
```

By using the echo_client ws sub-command, with the -m (mute or no_send), we will display statistics on how many messages we can receive per second.

```
$ ws echo_client -m ws://localhost:8008
[2020-08-02 12:31:17.284] [info] ws_echo_client: connected
[2020-08-02 12:31:17.284] [info] Uri: /
[2020-08-02 12:31:17.284] [info] Headers:
[2020-08-02 12:31:17.284] [info] Connection: Upgrade
[2020-08-02 12:31:17.284] [info] Sec-WebSocket-Accept: byy/pMK2d0PtRwExaaiOnXJTQHo=
[2020-08-02 12:31:17.284] [info] Server: ixwebsocket/10.1.4 macos ssl/SecureTransport zlib 1.2.11
[2020-08-02 12:31:17.284] [info] Upgrade: websocket
[2020-08-02 12:31:17.663] [info] messages received: 0 per second 2595307 total
[2020-08-02 12:31:18.668] [info] messages received: 79679 per second 2674986 total
[2020-08-02 12:31:19.668] [info] messages received: 207438 per second 2882424 total
[2020-08-02 12:31:20.673] [info] messages received: 209207 per second 3091631 total
[2020-08-02 12:31:21.676] [info] messages received: 216056 per second 3307687 total
[2020-08-02 12:31:22.680] [info] messages received: 214927 per second 3522614 total
[2020-08-02 12:31:23.684] [info] messages received: 216960 per second 3739574 total
[2020-08-02 12:31:24.688] [info] messages received: 215232 per second 3954806 total
[2020-08-02 12:31:25.691] [info] messages received: 212300 per second 4167106 total
[2020-08-02 12:31:26.694] [info] messages received: 212501 per second 4379607 total
[2020-08-02 12:31:27.699] [info] messages received: 212330 per second 4591937 total
[2020-08-02 12:31:28.702] [info] messages received: 216511 per second 4808448 total
```
