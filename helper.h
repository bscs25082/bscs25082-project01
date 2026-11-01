#ifndef HELPER_H
#define HELPER_H

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <unistd.h>
using namespace std;

//global variables
const int32_t maximumVariablesPerFrame = 16;
const int32_t maximumStackDepth = 64;
const int32_t maximumFunctions = 128;
const int32_t maximumTokens = maximumVariablesPerFrame + 2;
const int32_t maximumPatches = maximumFunctions * 4;
const uint64_t maximumSourceBytes = 15ULL * 1024 * 1024;
const int32_t ioBuffferSize = 64 * 1024;
const int32_t socketTimeOutSeconds = 5;

//shutdown flag
extern volatile sig_atomic_t flag;

//variable struct
struct variable
{
    string variableName;
    int32_t variableValue;
};

//frame struct
struct frame
{
    string functionName;
    int32_t argumentCount;
    variable argumentVector[maximumVariablesPerFrame];
    int32_t returnLine;
    variable locals[maximumVariablesPerFrame];
    int32_t localCount;
    variable *argumentRefernces[maximumVariablesPerFrame];
};

//snapshot structor
struct snapshot
{
    frame callStack[maximumStackDepth];
    int32_t stackDepth;
};

//TTDBHeader structor
struct TTDBHeader
{
    char magicArray[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

//function to update check sum
uint32_t update_check_sum(uint32_t cyclicRedundancyCheck, const char data[], int size);

class TrailerChecker
{
    private :
    char lastBytes[4];
    int savedBytes;
    uint32_t cyclicRedundancyCheck;
    uint64_t checkedBytes;

    public :
    //default constructor
    TrailerChecker();
    //function to feed
    int feed(const char data[], int size, char output[]);
    //function to verify
    bool verify() const;
    //function to return trailer
    const char *trailer() const;
};

//function to setup signal
void setup_signals();
//functin to read port
int read_port(const char text[]);
//function to configure socket
bool configure_socket(int socket);
//function to wait for socket
bool wait_socket(int socket, short event, int timeout);
//function to creater listen to socket
int create_listen_socket(int port);
//function to connect to host
int connect_to_host(const char *host, int port, int timeout = 1000);
//function to send all sockets
bool send_all(int socket, const char data[], int size, int timeout = 30000);
//function to recive sockets
int recive_some(int socket, char data[], int size, int timeout);
//function to show error
bool send_error(int socket, const string &errorMessage);
//funtion to send file
bool send_file(int socket, const char *path, bool &started);
//function to recive message
bool recive_message(int socket, FILE *output, int &status, string &error, uint64_t maximumBytes, int timeout = 30000);
//function to relay stream
bool relay_stream(int source, int destination, bool &started, int timeout);
//function to create session 
int create_session(const char *sourcePath, const char *resolvePath, const char *sessionPath);

#endif
