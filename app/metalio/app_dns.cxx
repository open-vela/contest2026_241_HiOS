#include "app_dns.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <netinet/in.h>
#include <netutils/netlib.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

namespace {
constexpr size_t kMaxServers = 5;
constexpr size_t kCacheEntries = 8;
constexpr unsigned kDnsPort = 53;
constexpr unsigned kTimeoutMs = 25000;
constexpr unsigned kRetryIntervalMs = 1800;
constexpr const char *kFallbackDns[] = {
    /* NT26's DHCP DNS proxy, kept app-local so the global resolver and radio
     * path are not changed. The modem currently leases this address on eth1. */
    "192.168.10.3",
    "223.5.5.5",
    "119.29.29.29"
};

struct CacheEntry {
    std::string host;
    uint32_t address{0};
    std::chrono::steady_clock::time_point expires{};
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
bool g_cellular = false;
bool g_logged_cellular_bind = false;
uint32_t g_servers[kMaxServers]{};
size_t g_server_count = 0;
uint16_t g_next_id = 0x4a20;
CacheEntry g_cache[kCacheEntries];
size_t g_cache_cursor = 0;

std::string normalize_host(const char *host)
{
    std::string out(host ? host : "");
    while (!out.empty() && out.back() == '.') out.pop_back();
    for (char &c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool cache_get(const std::string &host, uint32_t *address)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto &entry : g_cache)
        if (entry.address && entry.host == host && entry.expires > now)
        {
            *address = entry.address;
            return true;
        }
    return false;
}

void cache_put(const std::string &host, uint32_t address, uint32_t ttl)
{
    if (host.empty() || address == 0) return;
    if (ttl < 30) ttl = 30;
    if (ttl > 3600) ttl = 3600;
    CacheEntry &entry = g_cache[g_cache_cursor++ % kCacheEntries];
    entry.host = host;
    entry.address = address;
    entry.expires = std::chrono::steady_clock::now() +
                    std::chrono::seconds(ttl);
}

bool read_name(const uint8_t *msg, size_t length, size_t *offset,
               std::string *name)
{
    size_t pos = *offset;
    size_t resume = 0;
    size_t total = 0;
    bool jumped = false;
    name->clear();

    for (size_t steps = 0; steps < 128; ++steps)
    {
        if (pos >= length) return false;
        uint8_t label = msg[pos++];
        if ((label & 0xc0) == 0xc0)
        {
            if (pos >= length) return false;
            size_t target = ((size_t)(label & 0x3f) << 8) | msg[pos++];
            if (target >= length) return false;
            if (!jumped) resume = pos;
            jumped = true;
            pos = target;
            continue;
        }
        if (label & 0xc0) return false;
        if (label == 0)
        {
            *offset = jumped ? resume : pos;
            return true;
        }
        if (label > 63 || pos + label > length || total + label + 1 > 253)
            return false;
        if (!name->empty()) name->push_back('.');
        for (unsigned i = 0; i < label; ++i)
        {
            char c = static_cast<char>(msg[pos++]);
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            name->push_back(c);
        }
        total += label + 1;
    }
    return false;
}

bool parse_answer(const uint8_t *msg, size_t length, uint16_t *id,
                  std::string *question, uint32_t *address, uint32_t *ttl)
{
    if (length < 12) return false;
    *id = (uint16_t)((msg[0] << 8) | msg[1]);
    uint16_t flags = (uint16_t)((msg[2] << 8) | msg[3]);
    uint16_t qd = (uint16_t)((msg[4] << 8) | msg[5]);
    uint16_t an = (uint16_t)((msg[6] << 8) | msg[7]);
    if (!(flags & 0x8000) || (flags & 0x000f) != 0 || qd == 0) return false;

    size_t pos = 12;
    if (!read_name(msg, length, &pos, question) || pos + 4 > length)
        return false;
    pos += 4; /* QTYPE and QCLASS */

    for (uint16_t i = 0; i < an; ++i)
    {
        std::string rr_name;
        if (!read_name(msg, length, &pos, &rr_name) || pos + 10 > length)
            return false;
        uint16_t type = (uint16_t)((msg[pos] << 8) | msg[pos + 1]);
        uint16_t klass = (uint16_t)((msg[pos + 2] << 8) | msg[pos + 3]);
        uint32_t record_ttl = ((uint32_t)msg[pos + 4] << 24) |
                              ((uint32_t)msg[pos + 5] << 16) |
                              ((uint32_t)msg[pos + 6] << 8) | msg[pos + 7];
        uint16_t rdlen = (uint16_t)((msg[pos + 8] << 8) | msg[pos + 9]);
        pos += 10;
        if (pos + rdlen > length) return false;
        if (type == 1 && klass == 1 && rdlen == 4)
        {
            memcpy(address, msg + pos, 4);
            *ttl = record_ttl;
            return *address != 0;
        }
        pos += rdlen;
    }
    return false;
}

size_t make_query(uint8_t *msg, size_t capacity, uint16_t id,
                  const std::string &host)
{
    if (capacity < 17 || host.empty()) return 0;
    memset(msg, 0, 12);
    msg[0] = id >> 8; msg[1] = id;
    msg[2] = 1; /* recursion desired */
    msg[5] = 1; /* one question */
    size_t pos = 12;
    size_t begin = 0;
    while (begin < host.size())
    {
        size_t end = host.find('.', begin);
        if (end == std::string::npos) end = host.size();
        size_t label_len = end - begin;
        if (label_len == 0 || label_len > 63 || pos + label_len + 1 > capacity)
            return 0;
        msg[pos++] = static_cast<uint8_t>(label_len);
        memcpy(msg + pos, host.data() + begin, label_len);
        pos += label_len;
        begin = end + 1;
    }
    if (pos + 5 > capacity) return 0;
    msg[pos++] = 0;
    msg[pos++] = 0; msg[pos++] = 1; /* A */
    msg[pos++] = 0; msg[pos++] = 1; /* IN */
    return pos;
}

bool process_datagram(const uint8_t *buf, size_t len,
                      uint16_t current_id, const std::string &current_host,
                      uint32_t *current_address, uint32_t *ttl)
{
    uint16_t id = 0;
    std::string question;
    uint32_t address = 0;
    if (!parse_answer(buf, len, &id, &question, &address, ttl) ||
        id != current_id || question != current_host)
        return false;
    *current_address = address;
    return true;
}
}

extern "C" void metalio_app_dns_set_cellular(const uint32_t *servers,
                                               size_t count)
{
    pthread_mutex_lock(&g_lock);
    g_cellular = true;
    g_server_count = 0;
    for (size_t i = 0; servers && i < count && g_server_count < kMaxServers; ++i)
    {
        bool duplicate = false;
        for (size_t j = 0; j < g_server_count; ++j)
            duplicate |= g_servers[j] == servers[i];
        if (servers[i] && !duplicate) g_servers[g_server_count++] = servers[i];
    }
    for (const char *server_text : kFallbackDns)
    {
        if (g_server_count >= kMaxServers) break;
        struct in_addr parsed{};
        if (inet_pton(AF_INET, server_text, &parsed) != 1) continue;
        uint32_t server = parsed.s_addr;
        bool duplicate = false;
        for (size_t i = 0; i < g_server_count; ++i)
            duplicate |= g_servers[i] == server;
        if (!duplicate) g_servers[g_server_count++] = server;
    }
    pthread_mutex_unlock(&g_lock);
}

extern "C" void metalio_app_dns_set_wifi(void)
{
    pthread_mutex_lock(&g_lock);
    g_cellular = false;
    pthread_mutex_unlock(&g_lock);
}

extern "C" int metalio_app_bind_active_interface(int fd)
{
    pthread_mutex_lock(&g_lock);
    const bool cellular = g_cellular;
    const bool log_binding = cellular && !g_logged_cellular_bind;
    if (cellular) g_logged_cellular_bind = true;
    pthread_mutex_unlock(&g_lock);
    if (!cellular) return 0;

    static const char interface_name[] = "eth1";
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name,
                   sizeof(interface_name)) == 0)
    {
        struct in_addr local_address{};
        if (netlib_get_ipv4addr(interface_name, &local_address) < 0 ||
            local_address.s_addr == 0)
        {
            const int error = errno ? errno : EADDRNOTAVAIL;
            fprintf(stderr, "APPNET eth1 has no IPv4 address fd=%d errno=%d\n",
                    fd, error);
            return -error;
        }

        struct sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr = local_address;
        if (bind(fd, reinterpret_cast<struct sockaddr *>(&local),
                 sizeof(local)) < 0)
        {
            const int error = errno;
            fprintf(stderr, "APPNET bind eth1 address failed fd=%d errno=%d\n",
                    fd, error);
            return -error;
        }

        if (log_binding)
        {
            char address[INET_ADDRSTRLEN]{};
            (void)inet_ntop(AF_INET, &local_address, address, sizeof(address));
            fprintf(stderr, "APPNET pinned app socket to eth1/%s\n", address);
        }
        return 0;
    }

    const int error = errno;
    fprintf(stderr, "APPNET bind eth1 failed fd=%d errno=%d\n", fd, error);
    return -error;
}

extern "C" int metalio_app_cellular_active(void)
{
    pthread_mutex_lock(&g_lock);
    const bool cellular = g_cellular;
    pthread_mutex_unlock(&g_lock);
    return cellular ? 1 : 0;
}

static int resolve_ipv4(const char *hostname, uint32_t *address, bool media, bool voice = false)
{
    if (!hostname || !address) return -EINVAL;
    std::string host = normalize_host(hostname);
    if (host.empty()) return -EINVAL;

    uint32_t servers[kMaxServers]{};
    size_t server_count = 0;
    bool cellular = false;
    uint16_t id = 0;
    pthread_mutex_lock(&g_lock);
    if (!g_cellular && !media)
    {
        pthread_mutex_unlock(&g_lock);
        return -ENETDOWN;
    }
    cellular = g_cellular;
    struct in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1)
    {
        *address = literal.s_addr;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (cellular && cache_get(host, address))
    {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    server_count = g_server_count;
    memcpy(servers, g_servers, server_count * sizeof(servers[0]));
    id = ++g_next_id;
    if (id == 0) id = ++g_next_id;
    pthread_mutex_unlock(&g_lock);

    /* Do not spend a DNS retry window while DHCP has installed an address but
     * the cellular default route is not ready yet. */
    struct in_addr local_address{};
    struct in_addr gateway{};
    const char *interface_name = cellular ? "eth1" : "eth0";
    if (netlib_get_ipv4addr(interface_name, &local_address) < 0 ||
        local_address.s_addr == 0)
        return -EADDRNOTAVAIL;
    if (netlib_get_dripv4addr(interface_name, &gateway) < 0 ||
        gateway.s_addr == 0)
        return -ENETUNREACH;

    if (!cellular) {
        server_count = 0;
        servers[server_count++] = gateway.s_addr;
        const char *fallbacks[] = {"223.5.5.5", "119.29.29.29"};
        for (const char *text : fallbacks) {
            struct in_addr parsed{};
            if (inet_pton(AF_INET, text, &parsed) == 1 && parsed.s_addr != gateway.s_addr)
                servers[server_count++] = parsed.s_addr;
        }
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -errno;
    const int bind_result = metalio_app_bind_active_interface(fd);
    if (bind_result < 0)
    {
        close(fd);
        return bind_result;
    }
    /* The cellular helper already binds this socket to eth1's leased source
     * address. Binding again to INADDR_ANY can replace or invalidate the
     * ephemeral source port, so DNS replies then arrive at no matching socket.
     * Wi-Fi mode still needs the ordinary wildcard bind here. */
    if (!cellular)
    {
        struct sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr = local_address;
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0)
        {
            int err = errno;
            close(fd);
            return -err;
        }
    }

    uint8_t query[512];
    size_t query_len = make_query(query, sizeof(query), id, host);
    if (query_len == 0)
    {
        close(fd);
        return -EINVAL;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(voice ? 2000 : (media ? 10000 : kTimeoutMs));
    bool found = false;
    uint32_t result = 0;
    uint8_t response[512];
    unsigned attempts = 0;
    unsigned received = 0;
    unsigned dns_responses = 0;
    int last_rcode = -1;
    int resolve_error = -ETIMEDOUT;
    auto next_retry = std::chrono::steady_clock::time_point::min();
    while (std::chrono::steady_clock::now() < deadline && !found)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_retry)
        {
            if (server_count > 0)
            {
                /* Spread retries across resolvers. Sending every retry to
                 * every server creates bursts on the lossy cellular link and
                 * increases contention for the NT26 packet receive queue. */
                const size_t i = attempts % server_count;
                struct sockaddr_in server{};
                server.sin_family = AF_INET;
                server.sin_port = htons(kDnsPort);
                server.sin_addr.s_addr = servers[i];
                if (sendto(fd, query, query_len, 0,
                           (struct sockaddr *)&server, sizeof(server)) < 0)
                    resolve_error = -errno;
            }
            ++attempts;
            next_retry = now + std::chrono::milliseconds(voice ? 500 : kRetryIntervalMs);
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        auto retry_in = std::chrono::duration_cast<std::chrono::milliseconds>(
            next_retry - now).count();
        if (retry_in < remaining) remaining = retry_in;
        struct pollfd pfd{fd, POLLIN, 0};
        int ready = poll(&pfd, 1, remaining > 1000 ? 1000 : (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(pfd.revents & POLLIN)) continue;
        ssize_t n = recvfrom(fd, response, sizeof(response), 0,
                             nullptr, nullptr);
        if (n > 0)
        {
            ++received;
            if (n >= 12 && (response[2] & 0x80))
            {
                ++dns_responses;
                last_rcode = response[3] & 0x0f;
            }
            uint32_t ttl = 0;
            if (process_datagram(response, (size_t)n, id, host, &result, &ttl))
            {
                found = true;
                pthread_mutex_lock(&g_lock);
                if (cellular) cache_put(host, result, ttl);
                pthread_mutex_unlock(&g_lock);
            }
        }
    }

    close(fd);
    if (found) *address = result;
    char status[160];
    if (found)
    {
        char ip[INET_ADDRSTRLEN] = {};
        (void)inet_ntop(AF_INET, &result, ip, sizeof(ip));
        int n = snprintf(status, sizeof(status),
                         "APPDNS OK host=%s ip=%s tries=%u\n",
                         host.c_str(), ip, attempts);
        if (n > 0) (void)write(1, status, n < (int)sizeof(status) ? n : (int)sizeof(status)-1);
    }
    else
    {
        int n = snprintf(status, sizeof(status),
                         "APPDNS TIMEOUT host=%s tries=%u servers=%u rx=%u dns=%u rcode=%d err=%d\n",
                         host.c_str(), attempts, (unsigned)server_count,
                         received, dns_responses, last_rcode, resolve_error);
        if (n > 0) (void)write(1, status, n < (int)sizeof(status) ? n : (int)sizeof(status)-1);
    }
    return found ? 0 : -ETIMEDOUT;
}

extern "C" int metalio_app_resolve_ipv4(const char *host, uint32_t *address)
{
    return resolve_ipv4(host, address, false);
}

// Radio opts into a bounded resolver on the selected network. Existing app
// callers retain the original cellular-only behavior above.
extern "C" int metalio_media_resolve_ipv4(const char *host, uint32_t *address)
{
    return resolve_ipv4(host, address, true);
}

extern "C" int metalio_voice_resolve_ipv4(const char *host, uint32_t *address)
{
    return resolve_ipv4(host, address, true, true);
}
