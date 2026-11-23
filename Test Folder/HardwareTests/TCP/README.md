# TCP link test

Files: `TcpTest.c` and `Makefile`. Build ARM64 with `make`, laptop-native with `make native`. This is a small test server/client using port 9090, not the real application's 8080 server. It sends one known command and one fixed test response; it does not run DSP or control your system.

Pi terminal:

```sh
cd /tmp/phonostethos-tests/HardwareTests/TCP
./TcpTest server
```

Within 30 seconds, run on the laptop from this source folder:

```sh
./build-host/TcpTest client 10.42.0.3
```

Replace the IP if required. Both sides should print PASS. To change the port, supply it on both commands, for example `server 9091` and `client 10.42.0.3 9091`.

The client sends `{2, 0, 0, 0, 101}` in two submissions: SET_GAIN with code 101 (0.5x). The server assembles all five bytes and replies `{2, 1, 1, 1, 0}`, the fixed Status shape. These demonstration bytes are not an assertion that storage is actually present. The real protocol implementation is tested separately in SystemTests/Protocol.

Main APIs: socket creates an endpoint; bind/listen starts the listener; poll waits for a client; accept creates its descriptor; connect opens a client connection; send/recv may transfer fewer bytes than requested, so loops retain offsets; close releases sockets.

Laptop-only loopback check: run `./build-host/TcpTest server` in one terminal and `./build-host/TcpTest client 127.0.0.1` in another. This proves local socket exchange, not the physical laptop/Pi link.

No connection -> check both IP addresses, the interface, reachability, matching ports and firewall. Address already in use -> stop the previous test or choose another test port. Keep the test separate from the real GUI/server.
