#include "port/netplay.h"

extern "C" {
#include "game/gamework_data.h"
#include "game/frand.h"
#include "game/object.h"
#include "game/pad.h"
extern s32 rnd_seed;
extern u32 boardRandSeed;
}
#include "partyboard_version.h"
#include "port/settings.h"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr u16 DEFAULT_PORT = 46040;
constexpr u32 DEFAULT_INPUT_DELAY = 4;
constexpr u32 MAX_INPUT_DELAY = 30;
constexpr u32 PROTOCOL_MAGIC = 0x50424E50; // PBNP
constexpr u16 PROTOCOL_VERSION = 3;
constexpr size_t HEADER_SIZE = 12;
constexpr size_t INPUT_SIZE = 15;
constexpr size_t MAX_PAYLOAD_SIZE = 64;
constexpr size_t INPUT_BUFFER_SIZE = 64;
constexpr size_t GAME_STATE_WORDS = 8;
using GameState = std::array<u32, GAME_STATE_WORDS>;
constexpr size_t INPUT_PAYLOAD_SIZE = 4 + INPUT_SIZE + GAME_STATE_WORDS * 4;
static_assert(INPUT_PAYLOAD_SIZE <= MAX_PAYLOAD_SIZE);

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket INVALID_NET_SOCKET = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket INVALID_NET_SOCKET = -1;
#endif

enum class Role {
    None,
    Host,
    Client,
};

enum class MessageType : u16 {
    Hello = 1,
    Welcome = 2,
    Input = 3,
    Reject = 5,
    Desync = 6,
};

enum class RejectionReason : u32 {
    BuildMismatch = 1,
    SettingsMismatch = 2,
};

struct NetplayConfig {
    Role role = Role::None;
    std::string address;
    u16 port = DEFAULT_PORT;
    u32 inputDelay = DEFAULT_INPUT_DELAY;
};

struct InputState {
    u16 button = 0;
    u16 buttonDown = 0;
    u16 buttonRepeat = 0;
    s8 stickX = 0;
    s8 stickY = 0;
    s8 substickX = 0;
    s8 substickY = 0;
    u8 triggerLeft = 0;
    u8 triggerRight = 0;
    u8 dStick = 0;
    u8 dStickRepeat = 0;
    s8 error = PAD_ERR_NONE;
};

struct BufferedInput {
    u32 frame = 0;
    InputState state;
    GameState checkpoint{};
    bool valid = false;
};

struct Packet {
    MessageType type = MessageType::Hello;
    size_t size = 0;
    std::array<u8, MAX_PAYLOAD_SIZE> payload{};
};

Role g_role = Role::None;
std::atomic<Socket> g_socket { INVALID_NET_SOCKET };
std::atomic_bool g_cancelRequested = false;
bool g_socketSystemInitialized = false;
bool g_active = false;
bool g_desynchronized = false;
u32 g_inputDelay = DEFAULT_INPUT_DELAY;
u32 g_randomState = 0;
std::string g_lastError;
std::array<BufferedInput, INPUT_BUFFER_SIZE> g_localInputs{};

void write_u16(u8 *data, u16 value)
{
    data[0] = static_cast<u8>(value >> 8);
    data[1] = static_cast<u8>(value);
}

void write_u32(u8 *data, u32 value)
{
    data[0] = static_cast<u8>(value >> 24);
    data[1] = static_cast<u8>(value >> 16);
    data[2] = static_cast<u8>(value >> 8);
    data[3] = static_cast<u8>(value);
}

u16 read_u16(const u8 *data)
{
    return static_cast<u16>((static_cast<u16>(data[0]) << 8) | data[1]);
}

u32 read_u32(const u8 *data)
{
    return (static_cast<u32>(data[0]) << 24) |
        (static_cast<u32>(data[1]) << 16) |
        (static_cast<u32>(data[2]) << 8) |
        static_cast<u32>(data[3]);
}

void hash_value(u32 &hash, u32 value)
{
    for (int i = 0; i < 4; ++i) {
        hash ^= static_cast<u8>(value);
        hash *= 16777619u;
        value >>= 8;
    }
}

u32 build_hash()
{
    u32 hash = 2166136261u;
    for (const char value : std::string_view(PARTY_BOARD_WC_REVISION)) {
        hash ^= static_cast<u8>(value);
        hash *= 16777619u;
    }
    return hash;
}

u32 gameplay_settings_hash()
{
    const partyboard::UserSettings &settings = partyboard::getSettings();
    u32 hash = 2166136261u;
    hash_value(hash, static_cast<u32>(settings.game.language.getValue()));
    hash_value(hash, settings.game.enableQuickTransform.getValue());
    hash_value(hash, settings.game.pauseOnFocusLost.getValue());
    hash_value(hash, settings.game.infiniteHearts.getValue());
    hash_value(hash, settings.game.unlockAllMinigames.getValue());
    hash_value(hash, settings.game.unlockBowsersGnarlyParty.getValue());
    hash_value(hash, settings.game.enableTurboKeybind.getValue());
    hash_value(hash, settings.game.speedrunMode.getValue());
    hash_value(hash, settings.game.recordingMode.getValue());
    hash_value(hash, settings.backend.skipBootSequence.getValue());
    return hash;
}

int socket_error()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void close_socket(Socket &socket)
{
    if (socket == INVALID_NET_SOCKET) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
    socket = INVALID_NET_SOCKET;
}

void interrupt_socket(Socket socket)
{
    if (socket == INVALID_NET_SOCKET) {
        return;
    }
#ifdef _WIN32
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

void close_global_socket()
{
    Socket socket = g_socket.exchange(INVALID_NET_SOCKET);
    interrupt_socket(socket);
    close_socket(socket);
}

void set_connection_error(const char *message)
{
    g_lastError = message;
}

bool initialize_socket_system()
{
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::fprintf(stderr, "[netplay] WSAStartup failed\n");
        set_connection_error("Could not initialize Windows networking.");
        return false;
    }
#endif
    g_socketSystemInitialized = true;
    return true;
}

void shutdown_socket_system()
{
#ifdef _WIN32
    if (g_socketSystemInitialized) {
        WSACleanup();
    }
#endif
    g_socketSystemInitialized = false;
}

bool configure_socket(Socket socket)
{
    const int enabled = 1;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char *>(&enabled), sizeof(enabled)) != 0) {
        std::fprintf(stderr, "[netplay] Failed to configure TCP_NODELAY (%d)\n", socket_error());
        set_connection_error("Could not configure the network connection.");
        return false;
    }
#ifdef SO_NOSIGPIPE
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        std::fprintf(stderr, "[netplay] Failed to configure SO_NOSIGPIPE (%d)\n", socket_error());
        return false;
    }
#endif
#ifdef _WIN32
    const DWORD timeout = 15000;
    const bool timeoutConfigured =
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0 &&
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0;
#else
    const timeval timeout = { 15, 0 };
    const bool timeoutConfigured =
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
    if (!timeoutConfigured) {
        std::fprintf(stderr, "[netplay] Failed to configure socket timeout (%d)\n", socket_error());
        set_connection_error("Could not configure the network connection.");
        return false;
    }
    return true;
}

bool send_all(const u8 *data, size_t size)
{
    while (size != 0) {
        const Socket socket = g_socket.load();
        if (socket == INVALID_NET_SOCKET) {
            set_connection_error("The network connection was closed.");
            return false;
        }
#ifdef _WIN32
        const int sent = send(socket, reinterpret_cast<const char *>(data), static_cast<int>(size), 0);
#else
#ifdef MSG_NOSIGNAL
        constexpr int sendFlags = MSG_NOSIGNAL;
#else
        constexpr int sendFlags = 0;
#endif
        const ssize_t sent = send(socket, data, size, sendFlags);
#endif
        if (sent <= 0) {
            std::fprintf(stderr, "[netplay] Send failed (%d)\n", socket_error());
            set_connection_error("The peer disconnected or did not respond.");
            return false;
        }
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool receive_all(u8 *data, size_t size)
{
    while (size != 0) {
        const Socket socket = g_socket.load();
        if (socket == INVALID_NET_SOCKET) {
            set_connection_error("The network connection was closed.");
            return false;
        }
#ifdef _WIN32
        const int received = recv(socket, reinterpret_cast<char *>(data), static_cast<int>(size), 0);
#else
        const ssize_t received = recv(socket, data, size, 0);
#endif
        if (received == 0) {
            std::fprintf(stderr, "[netplay] Peer disconnected; %zu bytes still expected\n", size);
            set_connection_error("The peer closed the connection before the complete packet arrived.");
            return false;
        }
        if (received < 0) {
            const int error = socket_error();
            char message[160];
            std::snprintf(message, sizeof(message), "Receive failed or timed out (socket error %d; %zu bytes still expected).", error, size);
            std::fprintf(stderr, "[netplay] %s\n", message);
            set_connection_error(message);
            return false;
        }
        data += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

bool send_packet(MessageType type, const u8 *payload, size_t payloadSize)
{
    if (payloadSize > MAX_PAYLOAD_SIZE) {
        return false;
    }

    std::array<u8, HEADER_SIZE + MAX_PAYLOAD_SIZE> data{};
    write_u32(&data[0], PROTOCOL_MAGIC);
    write_u16(&data[4], PROTOCOL_VERSION);
    write_u16(&data[6], static_cast<u16>(type));
    write_u32(&data[8], static_cast<u32>(payloadSize));
    if (payloadSize != 0) {
        std::memcpy(&data[HEADER_SIZE], payload, payloadSize);
    }
    return send_all(data.data(), HEADER_SIZE + payloadSize);
}

bool receive_packet(Packet &packet)
{
    std::array<u8, HEADER_SIZE> header{};
    if (!receive_all(header.data(), header.size())) {
        return false;
    }
    char message[160];
    if (read_u32(&header[0]) != PROTOCOL_MAGIC) {
        std::snprintf(message, sizeof(message), "Invalid packet magic: expected 0x%08x, received 0x%08x. Is this a Party Board host?",
            PROTOCOL_MAGIC, read_u32(&header[0]));
        std::fprintf(stderr, "[netplay] %s\n", message);
        set_connection_error(message);
        return false;
    }
    if (read_u16(&header[4]) != PROTOCOL_VERSION) {
        std::snprintf(message, sizeof(message), "Protocol version mismatch: local=%u, peer=%u. Both players must use the same netplay protocol.",
            PROTOCOL_VERSION, read_u16(&header[4]));
        std::fprintf(stderr, "[netplay] %s\n", message);
        set_connection_error(message);
        return false;
    }

    packet.type = static_cast<MessageType>(read_u16(&header[6]));
    packet.size = read_u32(&header[8]);
    if (packet.size > packet.payload.size()) {
        std::snprintf(message, sizeof(message), "Oversized packet: type=%u, payload=%zu bytes, maximum=%zu.",
            static_cast<u16>(packet.type), packet.size, packet.payload.size());
        std::fprintf(stderr, "[netplay] %s\n", message);
        set_connection_error(message);
        return false;
    }
    return packet.size == 0 || receive_all(packet.payload.data(), packet.size);
}

void reject_handshake(RejectionReason reason)
{
    std::array<u8, 12> rejection{};
    write_u32(&rejection[0], static_cast<u32>(reason));
    write_u32(&rejection[4], build_hash());
    write_u32(&rejection[8], gameplay_settings_hash());
    send_packet(MessageType::Reject, rejection.data(), rejection.size());
}

bool parse_u16(std::string_view text, u16 &value)
{
    unsigned int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed == 0 || parsed > 65535) {
        return false;
    }
    value = static_cast<u16>(parsed);
    return true;
}

bool parse_u32(std::string_view text, u32 &value)
{
    unsigned int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_endpoint(std::string_view text, std::string &address, u16 &port)
{
    if (text.empty()) {
        return false;
    }
    const size_t separator = text.rfind(':');
    if (separator == std::string_view::npos) {
        address = text;
        return true;
    }
    if (text.find(':') != separator || separator == 0 || separator + 1 == text.size()) {
        return false;
    }
    address = text.substr(0, separator);
    return parse_u16(text.substr(separator + 1), port);
}

bool parse_arguments(int argc, char **argv, NetplayConfig &config)
{
    constexpr std::string_view hostPrefix = "--netplay-host=";
    constexpr std::string_view connectPrefix = "--netplay-connect=";
    constexpr std::string_view delayPrefix = "--netplay-delay=";

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        Role requestedRole = Role::None;
        if (argument == "--netplay-host") {
            requestedRole = Role::Host;
        } else if (argument.starts_with(hostPrefix)) {
            requestedRole = Role::Host;
            if (!parse_u16(argument.substr(hostPrefix.size()), config.port)) {
                std::fprintf(stderr, "[netplay] Invalid host port: %.*s\n",
                    static_cast<int>(argument.size()), argument.data());
                return false;
            }
        } else if (argument.starts_with(connectPrefix)) {
            requestedRole = Role::Client;
            if (!parse_endpoint(argument.substr(connectPrefix.size()), config.address, config.port)) {
                std::fprintf(stderr, "[netplay] Invalid server address: %.*s\n",
                    static_cast<int>(argument.size()), argument.data());
                return false;
            }
        } else if (argument.starts_with(delayPrefix)) {
            if (!parse_u32(argument.substr(delayPrefix.size()), config.inputDelay) ||
                config.inputDelay > MAX_INPUT_DELAY) {
                std::fprintf(stderr, "[netplay] Input delay must be between 0 and %u frames\n", MAX_INPUT_DELAY);
                return false;
            }
            continue;
        } else {
            continue;
        }

        if (config.role != Role::None && config.role != requestedRole) {
            std::fprintf(stderr, "[netplay] Host and client modes cannot be used together\n");
            return false;
        }
        config.role = requestedRole;
    }
    return true;
}

u32 create_session_seed()
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    u32 seed = static_cast<u32>(now) ^ static_cast<u32>(static_cast<u64>(now) >> 32) ^ build_hash();
    return seed != 0 ? seed : 1;
}

bool socket_operation_pending(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS || error == EWOULDBLOCK;
#endif
}

bool socket_operation_interrupted(int error)
{
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

bool set_socket_blocking(Socket socket, bool blocking)
{
#ifdef _WIN32
    u_long nonBlocking = blocking ? 0 : 1;
    return ioctlsocket(socket, FIONBIO, &nonBlocking) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(socket, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK) == 0;
#endif
}

bool wait_for_client(Socket listener)
{
    while (!g_cancelRequested.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listener, &readSet);
        timeval timeout { 0, 100000 };
#ifdef _WIN32
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = select(listener + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready > 0) {
            return true;
        }
        if (ready < 0 && !socket_operation_interrupted(socket_error())) {
            return false;
        }
    }
    return false;
}

bool connect_to_address(Socket socket, const sockaddr *address, int addressSize, int &connectionError)
{
    connectionError = 0;
    if (!set_socket_blocking(socket, false)) {
        connectionError = socket_error();
        return false;
    }

    if (connect(socket, address, addressSize) != 0) {
        const int error = socket_error();
        if (!socket_operation_pending(error)) {
            connectionError = error;
            return false;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!g_cancelRequested.load() && std::chrono::steady_clock::now() < deadline) {
        fd_set writeSet;
        fd_set errorSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(socket, &writeSet);
        FD_SET(socket, &errorSet);
        timeval timeout { 0, 100000 };
#ifdef _WIN32
        const int ready = select(0, nullptr, &writeSet, &errorSet, &timeout);
#else
        const int ready = select(socket + 1, nullptr, &writeSet, &errorSet, &timeout);
#endif
        if (ready < 0) {
            const int error = socket_error();
            if (socket_operation_interrupted(error)) {
                continue;
            }
            connectionError = error;
            return false;
        }
        if (ready == 0) {
            continue;
        }

        int socketConnectError = 0;
#ifdef _WIN32
        int connectErrorSize = sizeof(socketConnectError);
#else
        socklen_t connectErrorSize = sizeof(socketConnectError);
#endif
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char *>(&socketConnectError), &connectErrorSize) != 0) {
            connectionError = socket_error();
            return false;
        }
        if (socketConnectError != 0) {
            connectionError = socketConnectError;
            return false;
        }
        if (!set_socket_blocking(socket, true)) {
            connectionError = socket_error();
            return false;
        }
        return true;
    }
    if (!g_cancelRequested.load()) {
#ifdef _WIN32
        connectionError = WSAETIMEDOUT;
#else
        connectionError = ETIMEDOUT;
#endif
    }
    return false;
}

bool initialize_host(const NetplayConfig &config)
{
    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_NET_SOCKET) {
        std::fprintf(stderr, "[netplay] Failed to create host socket (%d)\n", socket_error());
        set_connection_error("Could not create the host socket.");
        return false;
    }

    const int reuseAddress = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char *>(&reuseAddress), sizeof(reuseAddress));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(config.port);
    if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        std::fprintf(stderr, "[netplay] Failed to listen on TCP port %u (%d)\n", config.port, socket_error());
        set_connection_error("Could not open the selected TCP port. It may already be in use.");
        close_socket(listener);
        return false;
    }

    std::fprintf(stderr, "[netplay] Hosting on TCP port %u; waiting for one client\n", config.port);
    std::fflush(stderr);

    sockaddr_in peerAddress{};
#ifdef _WIN32
    int peerAddressSize = sizeof(peerAddress);
#else
    socklen_t peerAddressSize = sizeof(peerAddress);
#endif
    Socket accepted = INVALID_NET_SOCKET;
    if (wait_for_client(listener)) {
        accepted = accept(listener, reinterpret_cast<sockaddr *>(&peerAddress), &peerAddressSize);
    }
    close_socket(listener);
    if (g_cancelRequested.load()) {
        close_socket(accepted);
        set_connection_error("Connection cancelled.");
        return false;
    }
    if (accepted == INVALID_NET_SOCKET) {
        std::fprintf(stderr, "[netplay] Failed to accept client (%d)\n", socket_error());
        set_connection_error("Could not accept the client connection.");
        return false;
    }
    g_socket.store(accepted);
    if (!configure_socket(accepted)) {
        return false;
    }

    Packet hello;
    if (!receive_packet(hello)) {
        std::fprintf(stderr, "[netplay] Could not receive client handshake: %s\n", g_lastError.c_str());
        return false;
    }
    if (hello.type != MessageType::Hello || hello.size != 8) {
        std::fprintf(stderr, "[netplay] Invalid client handshake: expected Hello (type=1, payload=8 bytes), received type=%u, payload=%zu bytes\n",
            static_cast<u16>(hello.type), hello.size);
        set_connection_error("The client sent an invalid handshake packet type or size.");
        return false;
    }
    const u32 clientBuild = read_u32(&hello.payload[0]);
    const u32 clientSettings = read_u32(&hello.payload[4]);
    std::fprintf(stderr, "[netplay] Client handshake: build_hash=0x%08x, settings_hash=0x%08x\n", clientBuild, clientSettings);
    if (clientBuild != build_hash()) {
        std::fprintf(stderr, "[netplay] Build mismatch: host=0x%08x, client=0x%08x. Both players must rebuild from the same Git commit.\n",
            build_hash(), clientBuild);
        reject_handshake(RejectionReason::BuildMismatch);
        set_connection_error("Build mismatch. Both players must rebuild from the same Git commit.");
        return false;
    }
    if (clientSettings != gameplay_settings_hash()) {
        std::fprintf(stderr, "[netplay] Gameplay settings mismatch: host=0x%08x, client=0x%08x. Compare the 'Hashed settings' lines on both peers.\n",
            gameplay_settings_hash(), clientSettings);
        reject_handshake(RejectionReason::SettingsMismatch);
        set_connection_error("Gameplay settings mismatch. Compare the 'Hashed settings' lines on both peers.");
        return false;
    }

    g_inputDelay = config.inputDelay;
    g_randomState = create_session_seed();
    std::array<u8, 12> welcome{};
    write_u32(&welcome[0], build_hash());
    write_u32(&welcome[4], g_inputDelay);
    write_u32(&welcome[8], g_randomState);
    if (!send_packet(MessageType::Welcome, welcome.data(), welcome.size())) {
        return false;
    }

    std::fprintf(stderr, "[netplay] Client connected; input delay is %u frames\n", g_inputDelay);
    return true;
}

bool initialize_client(const NetplayConfig &config)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *addresses = nullptr;
    const std::string port = std::to_string(config.port);
    const int lookupResult = getaddrinfo(config.address.c_str(), port.c_str(), &hints, &addresses);
    if (lookupResult != 0) {
        std::fprintf(stderr, "[netplay] Could not resolve host '%s'\n", config.address.c_str());
        set_connection_error("Could not resolve the host address.");
        return false;
    }

    Socket connected = INVALID_NET_SOCKET;
    int connectionError = 0;
    for (const addrinfo *address = addresses;
         address != nullptr && !g_cancelRequested.load(); address = address->ai_next) {
        Socket candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_NET_SOCKET) {
            connectionError = socket_error();
            continue;
        }
        int candidateError = 0;
        if (connect_to_address(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen), candidateError)) {
            connected = candidate;
            break;
        }
        connectionError = candidateError;
        close_socket(candidate);
    }
    freeaddrinfo(addresses);

    if (g_cancelRequested.load()) {
        close_socket(connected);
        set_connection_error("Connection cancelled.");
        return false;
    }
    if (connected == INVALID_NET_SOCKET) {
        std::fprintf(stderr, "[netplay] Could not connect to %s:%u (%d)\n",
            config.address.c_str(), config.port, connectionError);
        char errorMessage[160];
        std::snprintf(errorMessage, sizeof(errorMessage),
            "Could not connect to the host (socket error %d). Check the address, port, and firewall.",
            connectionError);
        set_connection_error(errorMessage);
        return false;
    }
    g_socket.store(connected);
    if (!configure_socket(connected)) {
        return false;
    }

    std::array<u8, 8> hello{};
    write_u32(&hello[0], build_hash());
    write_u32(&hello[4], gameplay_settings_hash());
    if (!send_packet(MessageType::Hello, hello.data(), hello.size())) {
        return false;
    }

    Packet welcome;
    if (!receive_packet(welcome)) {
        std::fprintf(stderr, "[netplay] Could not receive host handshake: %s No welcome/rejection received; check the host console for a build/settings rejection or cancellation.\n",
            g_lastError.c_str());
        return false;
    }
    if (welcome.type == MessageType::Reject && welcome.size == 12) {
        const u32 reason = read_u32(&welcome.payload[0]);
        const char *message = reason == static_cast<u32>(RejectionReason::BuildMismatch)
            ? "Build mismatch. Both players must rebuild from the same Git commit."
            : reason == static_cast<u32>(RejectionReason::SettingsMismatch)
                ? "Gameplay settings mismatch. Compare the 'Hashed settings' lines on both peers."
                : "The host rejected the connection with an unknown reason.";
        std::fprintf(stderr, "[netplay] Host rejected handshake (reason=%u): %s\n", reason, message);
        std::fprintf(stderr, "[netplay] Build hashes: host=0x%08x, client=0x%08x; gameplay settings hashes: host=0x%08x, client=0x%08x\n",
            read_u32(&welcome.payload[4]), build_hash(), read_u32(&welcome.payload[8]), gameplay_settings_hash());
        set_connection_error(message);
        return false;
    }
    if (welcome.type != MessageType::Welcome || welcome.size != 12) {
        std::fprintf(stderr, "[netplay] Invalid host handshake: expected Welcome (type=2, payload=12 bytes) or Reject (type=5, payload=12 bytes), received type=%u, payload=%zu bytes\n",
            static_cast<u16>(welcome.type), welcome.size);
        set_connection_error("The host sent an invalid handshake packet type or size.");
        return false;
    }
    if (read_u32(&welcome.payload[0]) != build_hash()) {
        std::fprintf(stderr, "[netplay] Build mismatch: host=0x%08x, client=0x%08x. Both players must rebuild from the same Git commit.\n",
            read_u32(&welcome.payload[0]), build_hash());
        set_connection_error("Build mismatch. Both players must rebuild from the same Git commit.");
        return false;
    }
    g_inputDelay = read_u32(&welcome.payload[4]);
    g_randomState = read_u32(&welcome.payload[8]);
    if (g_inputDelay > MAX_INPUT_DELAY || g_randomState == 0) {
        std::fprintf(stderr, "[netplay] Host sent invalid session settings: input_delay=%u (maximum=%u), random_seed=0x%08x (must be nonzero)\n",
            g_inputDelay, MAX_INPUT_DELAY, g_randomState);

        set_connection_error("The host sent invalid session settings.");
        return false;
    }

    std::fprintf(stderr, "[netplay] Connected to %s:%u; controlling player 2 with %u frames of input delay\n",
        config.address.c_str(), config.port, g_inputDelay);
    return true;
}

InputState capture_local_input()
{
    InputState state;
    state.button = HuPadBtn[0];
    state.buttonDown = HuPadBtnDown[0];
    state.buttonRepeat = HuPadBtnRep[0];
    state.stickX = HuPadStkX[0];
    state.stickY = HuPadStkY[0];
    state.substickX = HuPadSubStkX[0];
    state.substickY = HuPadSubStkY[0];
    state.triggerLeft = HuPadTrigL[0];
    state.triggerRight = HuPadTrigR[0];
    state.dStick = HuPadDStk[0];
    state.dStickRepeat = HuPadDStkRep[0];
    state.error = PAD_ERR_NONE;
    return state;
}

void encode_input(u8 *data, const InputState &state)
{
    write_u16(&data[0], state.button);
    write_u16(&data[2], state.buttonDown);
    write_u16(&data[4], state.buttonRepeat);
    data[6] = static_cast<u8>(state.stickX);
    data[7] = static_cast<u8>(state.stickY);
    data[8] = static_cast<u8>(state.substickX);
    data[9] = static_cast<u8>(state.substickY);
    data[10] = state.triggerLeft;
    data[11] = state.triggerRight;
    data[12] = state.dStick;
    data[13] = state.dStickRepeat;
    data[14] = static_cast<u8>(state.error);
}

InputState decode_input(const u8 *data)
{
    InputState state;
    state.button = read_u16(&data[0]);
    state.buttonDown = read_u16(&data[2]);
    state.buttonRepeat = read_u16(&data[4]);
    state.stickX = static_cast<s8>(data[6]);
    state.stickY = static_cast<s8>(data[7]);
    state.substickX = static_cast<s8>(data[8]);
    state.substickY = static_cast<s8>(data[9]);
    state.triggerLeft = data[10];
    state.triggerRight = data[11];
    state.dStick = data[12];
    state.dStickRepeat = data[13];
    state.error = static_cast<s8>(data[14]);
    return state;
}

void apply_input(int port, const InputState &state)
{
    HuPadBtn[port] = state.button;
    HuPadBtnDown[port] = state.buttonDown;
    HuPadBtnRep[port] = state.buttonRepeat;
    HuPadStkX[port] = state.stickX;
    HuPadStkY[port] = state.stickY;
    HuPadSubStkX[port] = state.substickX;
    HuPadSubStkY[port] = state.substickY;
    HuPadTrigL[port] = state.triggerLeft;
    HuPadTrigR[port] = state.triggerRight;
    HuPadDStk[port] = state.dStick;
    HuPadDStkRep[port] = state.dStickRepeat;
    HuPadErr[port] = state.error;
}

void apply_frame(u32 frame, const InputState &host, const InputState &client)
{
    apply_input(0, host);
    apply_input(1, client);

    InputState disconnected;
    disconnected.error = PAD_ERR_NO_CONTROLLER;
    apply_input(2, disconnected);
    apply_input(3, disconnected);

    if (frame % 600 == 0) {
        std::fprintf(stderr, "[netplay] Synchronized frame %u\n", frame);
    }
}

GameState capture_game_state()
{
    u32 players = 2166136261u;
    for (const auto &player : GWPlayerCfg) {
        hash_value(players, static_cast<u32>(player.character));
        hash_value(players, static_cast<u32>(player.pad_idx));
        hash_value(players, static_cast<u32>(player.diff));
        hash_value(players, static_cast<u32>(player.group));
        hash_value(players, static_cast<u32>(player.iscom));
    }
    u32 system = 2166136261u;
    for (u32 value : {static_cast<u32>(GWSystem.party), static_cast<u32>(GWSystem.team),
            static_cast<u32>(GWSystem.board), static_cast<u32>(GWSystem.turn),
            static_cast<u32>(GWSystem.player_curr), static_cast<u32>(GWSystem.mg_type),
            static_cast<u32>(GWSystem.mg_list), static_cast<u32>(GWSystem.explain_mg)}) {
        hash_value(system, value);
    }
    for (const auto &flags : GWSystem.flag) {
        for (u8 flag : flags) {
            hash_value(system, flag);
        }
    }
    return {static_cast<u32>(omcurovl), GWSystem.mg_next, static_cast<u32>(rnd_seed),
        frandStateGet(), boardRandSeed, g_randomState, players, system};
}

void store_local_input(u32 frame, const InputState &state, const GameState &checkpoint)
{
    BufferedInput &input = g_localInputs[frame % g_localInputs.size()];
    input.frame = frame;
    input.state = state;
    input.checkpoint = checkpoint;
    input.valid = true;
}

bool load_local_input(u32 frame, InputState &state, GameState &checkpoint)
{
    BufferedInput &input = g_localInputs[frame % g_localInputs.size()];
    if (!input.valid || input.frame != frame) {
        std::fprintf(stderr, "[netplay] Missing local input for frame %u\n", frame);
        return false;
    }
    state = input.state;
    checkpoint = input.checkpoint;
    input.valid = false;
    return true;
}

bool send_input(u32 frame, const InputState &state, const GameState &checkpoint)
{
    std::array<u8, INPUT_PAYLOAD_SIZE> payload{};
    write_u32(&payload[0], frame);
    encode_input(&payload[4], state);
    for (size_t i = 0; i < checkpoint.size(); ++i) {
        write_u32(&payload[4 + INPUT_SIZE + i * 4], checkpoint[i]);
    }
    return send_packet(MessageType::Input, payload.data(), payload.size());
}

bool receive_input(u32 expectedFrame, InputState &state, GameState &checkpoint)
{
    Packet packet;
    if (!receive_packet(packet)) {
        return false;
    }
    if (packet.type == MessageType::Desync && packet.size == 4) {
        g_desynchronized = true;
        send_packet(MessageType::Desync, packet.payload.data(), packet.size);
        set_connection_error("The peer detected a game-state desynchronization. Both players must restart.");
        std::fprintf(stderr, "[netplay] Peer reported desynchronization at game frame %u\n", read_u32(packet.payload.data()));
        return false;
    }
    if (packet.type != MessageType::Input || packet.size != INPUT_PAYLOAD_SIZE) {
        std::fprintf(stderr, "[netplay] Received an invalid input packet\n");
        return false;
    }
    const u32 frame = read_u32(&packet.payload[0]);
    if (frame != expectedFrame) {
        std::fprintf(stderr, "[netplay] Expected peer input for frame %u, received frame %u\n",
            expectedFrame, frame);
        return false;
    }
    state = decode_input(&packet.payload[4]);
    for (size_t i = 0; i < checkpoint.size(); ++i) {
        checkpoint[i] = read_u32(&packet.payload[4 + INPUT_SIZE + i * 4]);
    }
    return true;
}

bool synchronize_frame(u32 frame, const InputState &localInput)
{
    const GameState checkpoint = capture_game_state();
    store_local_input(frame + g_inputDelay, localInput, checkpoint);
    // Send future input before waiting, so both directions share the delay window.
    if (!send_input(frame + g_inputDelay, localInput, checkpoint)) {
        return false;
    }

    InputState local;
    InputState remote;
    GameState localCheckpoint{};
    GameState remoteCheckpoint{};
    if (frame >= g_inputDelay) {
        if (!load_local_input(frame, local, localCheckpoint) || !receive_input(frame, remote, remoteCheckpoint)) {
            return false;
        }
        if (localCheckpoint != remoteCheckpoint) {
            const u32 capturedFrame = frame - g_inputDelay;
            std::fprintf(stderr, "[netplay] Desynchronization at game frame %u (%s). Input frame numbers match, but game state differs.\n",
                capturedFrame, g_role == Role::Host ? "host" : "client");
            constexpr const char *names[] = {"overlay", "minigame", "rand8", "frand", "board RNG", "session RNG", "player configuration", "system flags"};
            for (size_t i = 0; i < localCheckpoint.size(); ++i) {
                if (localCheckpoint[i] != remoteCheckpoint[i]) {
                    std::fprintf(stderr, "[netplay] %s: local=0x%08x peer=0x%08x\n", names[i], localCheckpoint[i], remoteCheckpoint[i]);
                }
            }
            g_desynchronized = true;
            std::array<u8, 4> payload{};
            write_u32(payload.data(), capturedFrame);
            if (send_packet(MessageType::Desync, payload.data(), payload.size())) {
                // Keep the socket open until the peer acknowledges; closing here
                // can make its next input send fail before it reads the checkpoint.
                Packet acknowledgement;
                while (receive_packet(acknowledgement)) {
                    if (acknowledgement.type == MessageType::Desync && acknowledgement.size == 4) {
                        break;
                    }
                }
            }
            set_connection_error("Game state desynchronized. Both players must restart; compare the checkpoint details in both consoles.");
            return false;
        }
    }
    if (g_role == Role::Host) {
        apply_frame(frame, local, remote);
    } else {
        apply_frame(frame, remote, local);
    }
    return true;
}

bool initialize_connection(const NetplayConfig &config)
{
    g_lastError.clear();
    g_cancelRequested.store(false);
    if (g_active) {
        set_connection_error("A network session is already active.");
        return false;
    }
    if (!initialize_socket_system()) {
        return false;
    }

    g_desynchronized = false;
    g_role = config.role;
    std::fprintf(stderr, "[netplay] Local %s handshake: revision=%s, build_hash=0x%08x, settings_hash=0x%08x, protocol=%u\n",
        config.role == Role::Host ? "host" : "client", PARTY_BOARD_WC_REVISION, build_hash(), gameplay_settings_hash(), PROTOCOL_VERSION);
    const partyboard::UserSettings &settings = partyboard::getSettings();
    std::fprintf(stderr, "[netplay] Hashed settings: game.language=%u, game.enableQuickTransform=%u, game.pauseOnFocusLost=%u, game.infiniteHearts=%u, "
        "game.unlockAllMinigames=%u, game.unlockBowsersGnarlyParty=%u, game.enableTurboKeybind=%u, game.speedrunMode=%u, game.recordingMode=%u, backend.skipBootSequence=%u\n",
        static_cast<u32>(settings.game.language.getValue()), static_cast<u32>(settings.game.enableQuickTransform.getValue()),
        static_cast<u32>(settings.game.pauseOnFocusLost.getValue()), static_cast<u32>(settings.game.infiniteHearts.getValue()),
        static_cast<u32>(settings.game.unlockAllMinigames.getValue()), static_cast<u32>(settings.game.unlockBowsersGnarlyParty.getValue()),
        static_cast<u32>(settings.game.enableTurboKeybind.getValue()), static_cast<u32>(settings.game.speedrunMode.getValue()),
        static_cast<u32>(settings.game.recordingMode.getValue()), static_cast<u32>(settings.backend.skipBootSequence.getValue()));
    const bool initialized = config.role == Role::Host ? initialize_host(config) : initialize_client(config);
    if (!initialized || g_cancelRequested.load()) {
        if (g_cancelRequested.load()) {
            set_connection_error("Connection cancelled.");
        }
        partyboard_netplay_shutdown();
        return false;
    }

    g_active = true;
    return true;
}

} // namespace

extern "C" BOOL partyboard_netplay_initialize(int argc, char **argv)
{
    if (g_active) {
        return TRUE;
    }

    NetplayConfig config;
    if (!parse_arguments(argc, argv, config)) {
        return FALSE;
    }
    if (config.role == Role::None) {
        return TRUE;
    }
    return initialize_connection(config) ? TRUE : FALSE;
}

extern "C" BOOL partyboard_netplay_requested(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--netplay-host" || argument.starts_with("--netplay-host=") ||
            argument.starts_with("--netplay-connect=")) {
            return TRUE;
        }
    }
    return FALSE;
}

extern "C" BOOL partyboard_netplay_host(u16 port, u32 inputDelay)
{
    if (port == 0 || inputDelay > MAX_INPUT_DELAY) {
        set_connection_error("The port or input delay is invalid.");
        return FALSE;
    }
    NetplayConfig config;
    config.role = Role::Host;
    config.port = port;
    config.inputDelay = inputDelay;
    return initialize_connection(config) ? TRUE : FALSE;
}

extern "C" BOOL partyboard_netplay_join(const char *address, u16 port)
{
    if (address == nullptr || address[0] == '\0' || port == 0) {
        set_connection_error("The host address or port is invalid.");
        return FALSE;
    }
    NetplayConfig config;
    config.role = Role::Client;
    config.address = address;
    config.port = port;
    return initialize_connection(config) ? TRUE : FALSE;
}

extern "C" void partyboard_netplay_cancel_pending(void)
{
    g_cancelRequested.store(true);
    interrupt_socket(g_socket.load());
}

extern "C" const char *partyboard_netplay_last_error(void)
{
    return g_lastError.c_str();
}

extern "C" void partyboard_netplay_shutdown(void)
{
    g_active = false;
    close_global_socket();
    shutdown_socket_system();
    g_role = Role::None;
    g_localInputs = {};
}

extern "C" BOOL partyboard_netplay_active(void)
{
    return g_active ? TRUE : FALSE;
}

extern "C" BOOL partyboard_netplay_sync_frame(u32 frame)
{
    if (!g_active) {
        return TRUE;
    }

    const InputState localInput = capture_local_input();
    const bool synchronized = synchronize_frame(frame, localInput);
    if (!synchronized) {
        const Role disconnectedRole = g_role;
        const bool desynchronized = g_desynchronized;
        std::fprintf(stderr, "[netplay] Session ended at frame %u\n", frame);
        partyboard_netplay_shutdown();
        if (disconnectedRole == Role::Host && !desynchronized) {
            InputState disconnected;
            disconnected.error = PAD_ERR_NO_CONTROLLER;
            apply_input(1, disconnected);
            for (int i = 0; i < 4; ++i) {
                if (GWPlayerCfg[i].pad_idx == 1) {
                    GWPlayerCfg[i].iscom = TRUE;
                    GWPlayer[i].com = TRUE;
                }
            }
            std::fprintf(stderr, "[netplay] Client disconnected; continuing with player 2 as a computer\n");
            return TRUE;
        }
        return FALSE;
    }
    return TRUE;
}

extern "C" u32 partyboard_netplay_random_seed(void)
{
    if (!g_active) {
        return 0;
    }
    g_randomState = g_randomState * 1664525u + 1013904223u;
    return g_randomState != 0 ? g_randomState : 1;
}
