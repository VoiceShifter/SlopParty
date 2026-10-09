#include "port/netplay.h"

extern "C" {
#include "game/pad.h"
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
constexpr u16 PROTOCOL_VERSION = 1;
constexpr size_t HEADER_SIZE = 12;
constexpr size_t INPUT_SIZE = 15;
constexpr size_t MAX_PAYLOAD_SIZE = 64;
constexpr size_t INPUT_BUFFER_SIZE = 64;

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
    Frame = 4,
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
            std::fprintf(stderr, "[netplay] Peer disconnected\n");
            set_connection_error("The peer disconnected.");
            return false;
        }
        if (received < 0) {
            std::fprintf(stderr, "[netplay] Receive failed or timed out (%d)\n", socket_error());
            set_connection_error("The peer disconnected or did not respond.");
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
    if (read_u32(&header[0]) != PROTOCOL_MAGIC) {
        std::fprintf(stderr, "[netplay] Received a packet with an invalid magic value\n");
        return false;
    }
    if (read_u16(&header[4]) != PROTOCOL_VERSION) {
        std::fprintf(stderr, "[netplay] Peer uses a different protocol version\n");
        return false;
    }

    packet.type = static_cast<MessageType>(read_u16(&header[6]));
    packet.size = read_u32(&header[8]);
    if (packet.size > packet.payload.size()) {
        std::fprintf(stderr, "[netplay] Received an oversized packet\n");
        return false;
    }
    return packet.size == 0 || receive_all(packet.payload.data(), packet.size);
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

bool connect_to_address(Socket socket, const sockaddr *address, int addressSize)
{
    if (!set_socket_blocking(socket, false)) {
        return false;
    }

    if (connect(socket, address, addressSize) != 0 && !socket_operation_pending(socket_error())) {
        return false;
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
            if (socket_operation_interrupted(socket_error())) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            continue;
        }

        int connectError = 0;
#ifdef _WIN32
        int connectErrorSize = sizeof(connectError);
#else
        socklen_t connectErrorSize = sizeof(connectError);
#endif
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char *>(&connectError), &connectErrorSize) != 0 || connectError != 0) {
            return false;
        }
        return set_socket_blocking(socket, true);
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
    if (!receive_packet(hello) || hello.type != MessageType::Hello || hello.size != 8) {
        std::fprintf(stderr, "[netplay] Invalid client handshake\n");
        set_connection_error("The client sent an invalid handshake.");
        return false;
    }
    if (read_u32(&hello.payload[0]) != build_hash()) {
        std::fprintf(stderr, "[netplay] Client build does not match the host build\n");
        set_connection_error("The client is using a different Party Board build.");
        return false;
    }
    if (read_u32(&hello.payload[4]) != gameplay_settings_hash()) {
        std::fprintf(stderr, "[netplay] Client gameplay settings do not match the host settings\n");
        set_connection_error("The client's gameplay settings do not match the host's settings.");
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
    for (const addrinfo *address = addresses;
         address != nullptr && !g_cancelRequested.load(); address = address->ai_next) {
        Socket candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_NET_SOCKET) {
            continue;
        }
        if (connect_to_address(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen))) {
            connected = candidate;
            break;
        }
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
            config.address.c_str(), config.port, socket_error());
        set_connection_error("Could not connect to the host. Check the address, port, and firewall.");
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
    if (!receive_packet(welcome) || welcome.type != MessageType::Welcome || welcome.size != 12) {
        std::fprintf(stderr, "[netplay] Invalid host handshake\n");
        set_connection_error("The host sent an invalid handshake.");
        return false;
    }
    if (read_u32(&welcome.payload[0]) != build_hash()) {
        std::fprintf(stderr, "[netplay] Host build does not match this build\n");
        set_connection_error("The host is using a different Party Board build.");
        return false;
    }
    g_inputDelay = read_u32(&welcome.payload[4]);
    g_randomState = read_u32(&welcome.payload[8]);
    if (g_inputDelay > MAX_INPUT_DELAY || g_randomState == 0) {
        std::fprintf(stderr, "[netplay] Host sent invalid session settings\n");
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

void store_local_input(u32 frame, const InputState &state)
{
    BufferedInput &input = g_localInputs[frame % g_localInputs.size()];
    input.frame = frame;
    input.state = state;
    input.valid = true;
}

bool load_local_input(u32 frame, InputState &state)
{
    BufferedInput &input = g_localInputs[frame % g_localInputs.size()];
    if (!input.valid || input.frame != frame) {
        std::fprintf(stderr, "[netplay] Missing local input for frame %u\n", frame);
        return false;
    }
    state = input.state;
    input.valid = false;
    return true;
}

bool send_client_input(u32 frame, const InputState &state)
{
    std::array<u8, 4 + INPUT_SIZE> payload{};
    write_u32(&payload[0], frame);
    encode_input(&payload[4], state);
    return send_packet(MessageType::Input, payload.data(), payload.size());
}

bool receive_client_input(u32 expectedFrame, InputState &state)
{
    Packet packet;
    if (!receive_packet(packet) || packet.type != MessageType::Input || packet.size != 4 + INPUT_SIZE) {
        std::fprintf(stderr, "[netplay] Received an invalid input packet\n");
        return false;
    }
    const u32 frame = read_u32(&packet.payload[0]);
    if (frame != expectedFrame) {
        std::fprintf(stderr, "[netplay] Expected client input for frame %u, received frame %u\n",
            expectedFrame, frame);
        return false;
    }
    state = decode_input(&packet.payload[4]);
    return true;
}

bool send_frame(u32 frame, const InputState &host, const InputState &client)
{
    std::array<u8, 4 + INPUT_SIZE * 2> payload{};
    write_u32(&payload[0], frame);
    encode_input(&payload[4], host);
    encode_input(&payload[4 + INPUT_SIZE], client);
    return send_packet(MessageType::Frame, payload.data(), payload.size());
}

bool receive_frame(u32 expectedFrame, InputState &host, InputState &client)
{
    Packet packet;
    if (!receive_packet(packet) || packet.type != MessageType::Frame || packet.size != 4 + INPUT_SIZE * 2) {
        std::fprintf(stderr, "[netplay] Received an invalid frame packet\n");
        return false;
    }
    const u32 frame = read_u32(&packet.payload[0]);
    if (frame != expectedFrame) {
        std::fprintf(stderr, "[netplay] Expected synchronized frame %u, received frame %u\n",
            expectedFrame, frame);
        return false;
    }
    host = decode_input(&packet.payload[4]);
    client = decode_input(&packet.payload[4 + INPUT_SIZE]);
    return true;
}

bool synchronize_host(u32 frame, const InputState &localInput)
{
    store_local_input(frame + g_inputDelay, localInput);

    InputState host;
    InputState client;
    if (frame >= g_inputDelay) {
        if (!load_local_input(frame, host) || !receive_client_input(frame, client)) {
            return false;
        }
    }
    if (!send_frame(frame, host, client)) {
        return false;
    }
    apply_frame(frame, host, client);
    return true;
}

bool synchronize_client(u32 frame, const InputState &localInput)
{
    if (!send_client_input(frame + g_inputDelay, localInput)) {
        return false;
    }

    InputState host;
    InputState client;
    if (!receive_frame(frame, host, client)) {
        return false;
    }
    apply_frame(frame, host, client);
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

    g_role = config.role;
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
    const bool synchronized = g_role == Role::Host ?
        synchronize_host(frame, localInput) : synchronize_client(frame, localInput);
    if (!synchronized) {
        std::fprintf(stderr, "[netplay] Session ended at frame %u\n", frame);
        g_active = false;
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
