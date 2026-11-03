#include "helper.h"
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

volatile sig_atomic_t flag = 1;

//function to stop program
void stop_program(int signalNumber)
{
    (void)signalNumber;
    flag = 0;
}

//function to setup signals
void setup_signals()
{
    struct sigaction action = {};
    action.sa_handler = stop_program;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

//function to read port
int read_port(const char text[])
{
    int port = 0;
    if(text[0] == '\0')
        return -1;
    for(int p = 0; text[p] != '\0'; p++)
    {
        if(text[p] < '0' || text[p] > '9')
            return -1;
        port = port * 10 + text[p] - '0';
        if(port > 65535)
            return -1;
    }
    if(port > 0)
        return port;
    else
        return -1;
}

//function to configure socket
bool configure_socket(int sock)
{
    int options = fcntl(sock, F_GETFL, 0);
    if(options < 0 ||
       fcntl(sock, F_SETFL, options | O_NONBLOCK) < 0)
        return false;
    return fcntl(sock, F_SETFD, FD_CLOEXEC) != -1;
}

//function to wait for socket
bool wait_socket(int socket, short event, int timeout)
{
    while(flag && timeout > 0)
    {
        struct pollfd descriptor = {};
        descriptor.fd = socket;
        descriptor.events = event;
        int interval = timeout < 100 ? timeout : 100;
        int result = poll(&descriptor, 1, interval);
        if(result > 0)
        {
            if(descriptor.revents & POLLNVAL)
            {
                errno = EBADF;
                return false;
            }
            return true;
        }
        if(result < 0 && errno != EINTR)
            return false;
        if(result == 0)
            timeout = timeout - interval;
    }
    errno = flag ? ETIMEDOUT : EINTR;
    return false;
}

//functin to create listen socket
int create_listen_socket(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0)
        return -1;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if(!configure_socket(sock) || ::bind(sock, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(sock, 16) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}

//function to connect to host
int connect_to_host(const char *host, int port, int timeout)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0)
        return -1;
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if(strcmp(host, "localhost") == 0)
        host = "127.0.0.1";
    if(!configure_socket(sock) || inet_pton(AF_INET, host, &address.sin_addr) != 1)
    {
        close(sock);
        return -1;
    }
    int result = connect(sock, (struct sockaddr *)&address, sizeof(address));
    if(result < 0)
    {
        int error = 0;
        socklen_t size = sizeof(error);
        if(errno != EINPROGRESS || !wait_socket(sock, POLLOUT, timeout) || getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error != 0)
        {
            close(sock);
            return -1;
        }
    }
    return sock;
}

//function to send all
bool send_all(int sock, const char data[], int size, int timeout)
{
    int sent = 0;
    while(flag && sent < size)
    {
        if(!wait_socket(sock, POLLOUT, timeout))
            return false;
        ssize_t count = send(sock, data + sent, size - sent, 0);
        if(count > 0)
            sent = sent + (int)count;
        else if(count == 0 ||
                (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return false;
    }
    return sent == size;
}

//function to recieve some
int receive_some(int sock, char data[], int size, int timeout)
{
    while(flag)
    {
        if(!wait_socket(sock, POLLIN, timeout))
            return -1;
        ssize_t count = recv(sock, data, size, 0);
        if(count >= 0)
            return (int)count;
        if(errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
    }
    errno = EINTR;
    return -1;
}

//function to update check sum
uint32_t update_check_sum(uint32_t cyclicRedundancyCheck, const char data[], int size)
{
    for(int p = 0; p < size; p++)
    {
        cyclicRedundancyCheck ^= (unsigned char)data[p];
        for(int bit = 0; bit < 8; bit++)
        {
            if(cyclicRedundancyCheck & 1)
                cyclicRedundancyCheck = (cyclicRedundancyCheck >> 1) ^ 0xEDB88320U;
            else
                cyclicRedundancyCheck >>= 1;
        }
    }
    return cyclicRedundancyCheck;
}

//trailer constructor
TrailerChecker::TrailerChecker()
{
    savedBytes = 0;
    cyclicRedundancyCheck = 0xFFFFFFFFU;
    checkedBytes = 0;
}

//trailer checker feed function
int TrailerChecker::feed(const char dataArray[], int arraySize, char outputArray[])
{
    memcpy(outputArray, lastBytes, savedBytes);
    memcpy(outputArray + savedBytes, dataArray, arraySize);
    int totalBytes = savedBytes + arraySize;
    int readyBytes = totalBytes > 4 ? totalBytes - 4 : 0;
    savedBytes = totalBytes - readyBytes;
    memcpy(lastBytes, outputArray + readyBytes, savedBytes);
    cyclicRedundancyCheck = update_check_sum(cyclicRedundancyCheck, outputArray, readyBytes);
    checkedBytes = checkedBytes + readyBytes;
    return readyBytes;
}

//function to verify in class trailer
bool TrailerChecker::verify() const
{
    if(savedBytes != 4 || checkedBytes == 0)
        return false;
    uint32_t receivedBytes;
    memcpy(&receivedBytes, lastBytes, 4);
    return ntohl(receivedBytes) == (cyclicRedundancyCheck ^ 0xFFFFFFFFU);
}

//function to return trailer from class trailer checker
const char *TrailerChecker::trailer() const
{
    return lastBytes;
}

//function to send error
bool send_error(int socket, const string &message)
{
    char dataArray[4101];
    int length = message.size() > 4096 ? 4096 : (int)message.size();
    dataArray[0] = 1;
    memcpy(dataArray + 1, message.data(), length);
    uint32_t cyclicRedundancyCheck = update_check_sum(0xFFFFFFFFU, dataArray, length + 1);
    cyclicRedundancyCheck = htonl(cyclicRedundancyCheck ^ 0xFFFFFFFFU);
    memcpy(dataArray + length + 1, &cyclicRedundancyCheck, 4);
    bool success = send_all(socket, dataArray, length + 5, 1000);
    shutdown(socket, SHUT_WR);
    return success;
}

//function to send file
bool send_file(int socket, const char *path, bool &isStarted)
{
    isStarted = false;
    FILE *file = fopen(path, "rb");
    if(file == nullptr)
        return false;
    char status = 0;
    uint32_t cyclicRedundancyCheck = update_check_sum(0xFFFFFFFFU, &status, 1);
    isStarted = true;
    bool isSuccess = send_all(socket, &status, 1);
    char buffer[ioBuffferSize];
    while(isSuccess == true)
    {
        int count = (int)fread(buffer, 1, sizeof(buffer), file);
        if(count == 0)
        {
            isSuccess = !ferror(file);
            break;
        }
        cyclicRedundancyCheck = update_check_sum(cyclicRedundancyCheck, buffer, count);
        isSuccess = send_all(socket, buffer, count);
    }
    fclose(file);
    if(isSuccess == true)
    {
        cyclicRedundancyCheck = htonl(cyclicRedundancyCheck ^ 0xFFFFFFFFU);
        isSuccess = send_all(socket, (const char *)&cyclicRedundancyCheck, 4);
    }
    shutdown(socket, SHUT_WR);
    return isSuccess;
}

//function to receive message
bool receive_message(int socket, FILE *output, int &status, string &error, uint64_t maximumBytes, int timeout)
{
    TrailerChecker checker;
    char buffer[ioBuffferSize];
    char ready[ioBuffferSize + 4];
    uint64_t bodySize = 0;
    status = -1;
    error.clear();
    while(flag)
    {
        int count = receive_some(socket, buffer, sizeof(buffer), timeout);
        if(count <= 0)
        {
            if(count < 0 && !(errno == ECONNRESET && status == 1 && checker.verify()))
            {
                error = "connection failed or timed out";
                return false;
            }
            if(!checker.verify())
            {
                error = "checksum mismatch, response corrupted";
                return false;
            }
            return status == 0;
        }
        int length = checker.feed(buffer, count, ready);
        int begin = 0;
        if(status == -1 && length > 0)
        {
            status = (unsigned char)ready[0];
            begin = 1;
            if(status != 0 && status != 1)
            {
                error = "invalid message status";
                return false;
            }
        }
        int bytes = length - begin;
        if(bytes == 0)
            continue;
        bodySize = bodySize + bytes;
        if(status == 1)
        {
            if(bodySize > 4096)
            {
                error = "error message too large";
                return false;
            }
            error.append(ready + begin, bytes);
        }
        else if(bodySize > maximumBytes || fwrite(ready + begin, 1, bytes, output) != (size_t)bytes)
        {
            error = "file too large or cannot write file";
            return false;
        }
    }
    error = "program stopped";
    return false;
}

bool relay_stream(int source, int destination, bool &started, int timeout)
{
    TrailerChecker checker;
    char buffer[ioBuffferSize];
    char ready[ioBuffferSize + 4];
    while(flag)
    {
        int count = receive_some(source, buffer, sizeof(buffer), timeout);
        if(count < 0)
            return false;
        if(count == 0)
        {
            if(!checker.verify())
                return false;
            return send_all(destination, checker.trailer(), 4);
        }
        int length = checker.feed(buffer, count, ready);
        if(length > 0)
        {
            started = true;
            if(!send_all(destination, ready, length))
                return false;
        }
    }
    return false;
}
