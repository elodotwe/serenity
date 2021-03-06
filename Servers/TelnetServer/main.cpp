#include "Client.h"
#include <AK/BufferStream.h>
#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/IPv4Address.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Types.h>
#include <LibCore/CEventLoop.h>
#include <LibCore/CTCPServer.h>
#include <LibCore/CTCPSocket.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

static void run_command(int ptm_fd, String command)
{
    pid_t pid = fork();
    if (pid == 0) {
        const char* tty_name = ptsname(ptm_fd);
        if (!tty_name) {
            perror("ptsname");
            exit(1);
        }
        close(ptm_fd);
        int pts_fd = open(tty_name, O_RDWR);
        if (pts_fd < 0) {
            perror("open");
            exit(1);
        }

        // NOTE: It's okay if this fails.
        (void)ioctl(0, TIOCNOTTY);

        close(0);
        close(1);
        close(2);

        int rc = dup2(pts_fd, 0);
        if (rc < 0) {
            perror("dup2");
            exit(1);
        }
        rc = dup2(pts_fd, 1);
        if (rc < 0) {
            perror("dup2");
            exit(1);
        }
        rc = dup2(pts_fd, 2);
        if (rc < 0) {
            perror("dup2");
            exit(1);
        }
        rc = close(pts_fd);
        if (rc < 0) {
            perror("close");
            exit(1);
        }
        rc = ioctl(0, TIOCSCTTY);
        if (rc < 0) {
            perror("ioctl(TIOCSCTTY)");
            exit(1);
        }
        const char* args[4] = { "/bin/Shell", nullptr, nullptr, nullptr };
        if (!command.is_empty()) {
            args[1] = "-c";
            args[2] = command.characters();
        }
        const char* envs[] = { "TERM=xterm", "PATH=/bin:/usr/bin:/usr/local/bin", nullptr };
        rc = execve("/bin/Shell", const_cast<char**>(args), const_cast<char**>(envs));
        if (rc < 0) {
            perror("execve");
            exit(1);
        }
        ASSERT_NOT_REACHED();
    }
}

int main(int argc, char** argv)
{
    CEventLoop event_loop;
    auto server = CTCPServer::construct();

    int opt;
    u16 port = 23;
    const char* command = "";
    while ((opt = getopt(argc, argv, "p:c:")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            break;
        case 'c':
            command = optarg;
            break;
        default:
            fprintf(stderr, "Usage: %s [-p port] [-c command]", argv[0]);
            exit(1);
        }
    }

    if (!server->listen({}, port)) {
        perror("listen");
        exit(1);
    }

    HashMap<int, NonnullRefPtr<Client>> clients;
    int next_id = 0;

    server->on_ready_to_accept = [&next_id, &clients, &server, command] {
        int id = next_id++;

        auto client_socket = server->accept();
        if (!client_socket) {
            perror("accept");
            return;
        }

        int ptm_fd = open("/dev/ptmx", O_RDWR);
        if (ptm_fd < 0) {
            perror("open(ptmx)");
            client_socket->close();
            return;
        }

        run_command(ptm_fd, command);

        auto client = Client::create(id, move(client_socket), ptm_fd);
        client->on_exit = [&clients, id] { clients.remove(id); };
        clients.set(id, client);
    };

    int rc = event_loop.exec();
    if (rc != 0) {
        fprintf(stderr, "event loop exited badly; rc=%d", rc);
        exit(1);
    }

    return 0;
}
