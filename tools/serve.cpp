// serve: a tiny web server for an apt repository (on the build machine).
//
//   serve <folder> [port]       (default port 8080)
//
// Serves the files in <folder> over HTTP, so Bhairav OS (in QEMU, where the
// build machine is 10.0.2.2) can `apt add-repo http://10.0.2.2:8080`.
// Only for testing: one request at a time, files only, no listings.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: serve <folder> [port]\n");
        return 2;
    }
    std::string root = argv[1];
    int port = argc > 2 ? atoi(argv[2]) : 8080;

    int server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 or listen(server, 8) < 0) {
        perror("serve");
        return 1;
    }
    printf("serving %s on port %d\n", root.c_str(), port);
    fflush(stdout);

    for (;;) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0)
            continue;
        char request[2048] = {};
        ssize_t n = recv(client, request, sizeof(request) - 1, 0);
        // "GET /path HTTP/1.1"
        char path[1024] = {};
        bool ok = n > 0 and sscanf(request, "GET %1023s", path) == 1 and path[0] == '/' and
                  not strstr(path, "..");
        std::string file = root + path;
        FILE* f = ok ? fopen(file.c_str(), "rb") : nullptr;
        if (not f) {
            char const* missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 10\r\nConnection: close\r\n\r\nnot found\n";
            send(client, missing, strlen(missing), 0);
            printf("404 %s\n", path);
        } else {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            char header[256];
            int h = snprintf(header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nContent-Type: application/octet-stream\r\n"
                             "Connection: close\r\n\r\n",
                             size);
            send(client, header, static_cast<size_t>(h), 0);
            char buffer[65536];
            size_t got;
            while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
                send(client, buffer, got, 0);
            fclose(f);
            printf("200 %s (%ld bytes)\n", path, size);
        }
        fflush(stdout);
        shutdown(client, SHUT_WR);
        close(client);
    }
}
