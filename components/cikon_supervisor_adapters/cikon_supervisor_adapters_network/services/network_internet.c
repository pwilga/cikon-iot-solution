#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h" // IWYU pragma: keep

#include "bits_helper.h"
#include "network_services.h"
#include "supervisor.h"

#if CONFIG_NETWORK_UPLINK_THREAD
#include "thread_common.h"
#endif

#define TAG "cikon:network:internet"
#define INTERNET_CHECK_SRC_PORT 49152

bool network_services_is_internet_reachable(void) {
#if CONFIG_NETWORK_UPLINK_THREAD && CONFIG_OPENTHREAD_DNS64_CLIENT
    // Same as `ot ping 8.8.8.8`: Thread has no IPv4, the BR's NAT64 prefix gets glued to the
    // address
    char nat64_address[THREAD_NAT64_ADDRESS_MAX];
    if (!thread_nat64_address("8.8.8.8", nat64_address, sizeof(nat64_address))) {
        return false;
    }
    return network_services_is_tcp_port_reachable(nat64_address, 53);
#else
    return network_services_is_tcp_port_reachable("8.8.8.8", 53);
#endif
}

void network_services_poll_internet_reachability(void) {

    static bool last_internet_reachable = false;

    bool current_state = network_services_is_internet_reachable();
    if (current_state != last_internet_reachable) {
        supervisor_notify_event(current_state ? NETWORK_INTERNET_READY : NETWORK_INTERNET_LOST);
        last_internet_reachable = current_state;
    }
}

bool network_services_is_tcp_port_reachable(const char *host, uint16_t port) {
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));

    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
    socklen_t addr_len;

    // Resolve the host name may take time !. For real non blocking connect use ip address.
    if (inet_aton(host, &addr4->sin_addr) != 0) {
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        addr_len = sizeof(*addr4);
    } else if (inet_pton(AF_INET6, host, &addr6->sin6_addr) == 1) {
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        addr_len = sizeof(*addr6);
    } else {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            return false;
        }
        memcpy(&addr4->sin_addr, he->h_addr, he->h_length);
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        addr_len = sizeof(*addr4);
    }

    int sock = socket(addr.ss_family, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

#if CONFIG_LWIP_SO_LINGER
    // Same source port every time - a NAT64 on the way (Thread BR) keeps reusing one session
    // instead of opening a new one per check. Closing with RST frees the port right away
    struct linger lin = {.l_onoff = 1, .l_linger = 0};
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));

    struct sockaddr_storage local;
    memset(&local, 0, sizeof(local));
    local.ss_family = addr.ss_family;
    if (addr.ss_family == AF_INET6) {
        ((struct sockaddr_in6 *)&local)->sin6_port = htons(INTERNET_CHECK_SRC_PORT);
    } else {
        ((struct sockaddr_in *)&local)->sin_port = htons(INTERNET_CHECK_SRC_PORT);
    }
    if (bind(sock, (struct sockaddr *)&local, addr_len) != 0) {
        ESP_LOGW(TAG, "Internet check: bind to port %d failed: %s", INTERNET_CHECK_SRC_PORT,
                 strerror(errno));
    }
#endif

    int res = connect(sock, (struct sockaddr *)&addr, addr_len);
    if (res == 0) {
        close(sock);
        return true;
    } else if (errno != EINPROGRESS) {
        close(sock);
        return false;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 500 * 1000};
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    res = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (res > 0) {
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        close(sock);
        return so_error == 0;
    } else {
        close(sock);
        return false;
    }
}
