#include "helper.h"
#include <sstream>
#include <cstring>
#include <cctype>
#include <stdexcept>
#include <cstdlib>
#include <sys/socket.h>

//global constrains
string loadBalancerHost;
int loadBalancerPort;

//function for TTDB reader
struct timeTravelDebuggerReader
{
    FILE *fileName;
    TTDBHeader timeTravelHeader;
    int64_t *index;
};

//struct for debug session
struct debugSession
{
    bool isActive;
    timeTravelDebuggerReader reader;
    int64_t currentStep;
};

//function to close time travel debugger
void close_time_travel_debugger(timeTravelDebuggerReader &reader)
{
    if(reader.fileName != nullptr)
        fclose(reader.fileName);
    delete[] reader.index;
    reader.fileName = nullptr;
    reader.index = nullptr;
}

//function to read data
void read_data(FILE *fileName, void *output, size_t size, int64_t end)
{
    long position = ftell(fileName);
    if(position < 0 || position > end || size > (uint64_t)(end - position) || fread(output, 1, size, fileName) != size)
        throw runtime_error("damaged .tdbg file");
}

//function to read intergar
int32_t read_integer(FILE *fileName, int64_t end)
{
    int32_t value;
    read_data(fileName, &value, 4, end);
    return value;
}

//function to read name
string read_name(FILE *fileName, int64_t end)
{
    int32_t length = read_integer(fileName, end);
    long position = ftell(fileName);
    if(length <= 0 || (uint64_t)length > maximumSourceBytes || position < 0 || length > end - position)
        throw runtime_error("invalid name length");
    string name(length, '\0');
    read_data(fileName, &name[0], length, end);
    for(int p = 0; p < length; p++)
    {
        unsigned char letter = name[p];
        if(letter != '_' && !(p == 0 ? isalpha(letter) : isalnum(letter)))
            throw runtime_error("invalid name");
    }
    return name;
}

//function to read variables
int32_t read_variables(FILE *fileName, variable values[], int64_t end)
{
    int32_t count = read_integer(fileName, end);
    if(count < 0 || count > maximumVariablesPerFrame)
        throw runtime_error("invalid variable count");
    for(int p = 0; p < count; p++)
    {
        values[p].variableName = read_name(fileName, end);
        values[p].variableValue = read_integer(fileName, end);
        for(int q = 0; q < p; q++)
        {
            if(values[q].variableName == values[p].variableName)
                throw runtime_error("duplicate variable");
        }
    }
    return count;
}

//function to read snapshot at
snapshot *read_snapshot_at(timeTravelDebuggerReader &reader, int64_t step)
{
    if(step < 0 || step >= reader.timeTravelHeader.stepCount)
        return nullptr;
    snapshot *result = nullptr;
    try
    {
        int64_t end = reader.timeTravelHeader.indexOffset;
        if(step + 1 < reader.timeTravelHeader.stepCount)
            end = reader.index[step + 1];
        if(fseek(reader.fileName, (long)reader.index[step], SEEK_SET) != 0)
            throw runtime_error("cannot seek snapshot");
        result = new snapshot{};
        result->stackDepth = read_integer(reader.fileName, end);
        if(result->stackDepth < 0 || result->stackDepth > maximumStackDepth)
            throw runtime_error("invalid stack depth");
        for(int p = 0; p < result->stackDepth; p++)
        {
            frame &current = result->callStack[p];
            current.functionName = read_name(reader.fileName, end);
            current.argumentCount = read_variables(reader.fileName, current.argumentVector, end);
            current.returnLine = read_integer(reader.fileName, end);
            if(current.returnLine < -1)
                throw runtime_error("invalid return address");
            current.localCount = read_variables(reader.fileName, current.locals, end);
            for(int i = 0; i < current.argumentCount; i++)
            {
                for(int j = 0; j < current.localCount; j++)
                {
                    if(current.argumentVector[i].variableName == current.locals[j].variableName)
                        throw runtime_error("duplicate variable");
                }
            }
        }
        if(ftell(reader.fileName) != end)
            throw runtime_error("invalid snapshot size");
        return result;
    }
    catch(const exception &error)
    {
        delete result;
        return nullptr;
    }
}

//function to open time travel debugger
bool open_time_travel_debugger(const char *path, timeTravelDebuggerReader &reader)
{
    close_time_travel_debugger(reader);
    reader.fileName = fopen(path, "rb");
    if(reader.fileName == nullptr)
        return false;
    try
    {
        if(fseek(reader.fileName, 0, SEEK_END) != 0)
            throw runtime_error("file size error");
        int64_t size = ftell(reader.fileName);
        if(size < 20 || fseek(reader.fileName, 0, SEEK_SET) != 0)
            throw runtime_error("short header");
        read_data(reader.fileName, reader.timeTravelHeader.magicArray, 4, size);
        reader.timeTravelHeader.version = read_integer(reader.fileName, size);
        reader.timeTravelHeader.stepCount = read_integer(reader.fileName, size);
        read_data(reader.fileName, &reader.timeTravelHeader.indexOffset, 8, size);
        int64_t indexStart = reader.timeTravelHeader.indexOffset;
        int32_t count = reader.timeTravelHeader.stepCount;
        if(memcmp(reader.timeTravelHeader.magicArray, "TTDB", 4) != 0 || reader.timeTravelHeader.version != 1 || count <= 0 || indexStart < 20 || indexStart > size || size - indexStart != (int64_t)count * 8)
            throw runtime_error("invalid header");
        reader.index = new int64_t[count];
        if(fseek(reader.fileName, (long)indexStart, SEEK_SET) != 0)
            throw runtime_error("invalid index");
        read_data(reader.fileName, reader.index, (size_t)count * 8, size);
        for(int p = 0; p < count; p++)
        {
            if(reader.index[p] < 20 || reader.index[p] >= indexStart || (p == 0 && reader.index[p] != 20) || (p > 0 && reader.index[p] <= reader.index[p - 1]))
                throw runtime_error("invalid index");
        }
        for(int p = 0; p < count; p++)
        {
            snapshot *current = read_snapshot_at(reader, p);
            if(current == nullptr)
                throw runtime_error("invalid snapshot");
            delete current;
        }
        return true;
    }
    catch(const exception &error)
    {
        close_time_travel_debugger(reader);
        return false;
    }
}

//function to request debug file
bool request_debug_file(const string &sourcePath, const char *destination, string &error)
{
    FILE *source = fopen(sourcePath.c_str(), "rb");
    if(source == nullptr)
    {
        error = "cannot open source file";
        return false;
    }
    bool readable = fseek(source, 0, SEEK_END) == 0;
    long size = readable ? ftell(source) : -1;
    fclose(source);
    if(size < 0 || (uint64_t)size > maximumSourceBytes)
    {
        error = "source must be readable and at most 15 MiB";
        return false;
    }
    FILE *outputFile = fopen(destination, "wb");
    if(outputFile == nullptr)
    {
        error = "cannot create download file";
        return false;
    }
    int socket = connect_to_host(loadBalancerHost.c_str(), loadBalancerPort);
    if(socket < 0)
    {
        fclose(outputFile);
        error = "cannot connect to load balancer";
        return false;
    }
    bool isStarted;
    bool isUploaded = send_file(socket, sourcePath.c_str(), isStarted);
    shutdown(socket, SHUT_WR);
    int status;
    bool received = receive_message(socket, outputFile, status, error, UINT64_MAX);
    close(socket);
    int closed = fclose(outputFile);
    if(received && (!isUploaded || closed != 0))
    {
        error = "upload or saving download failed";
        return false;
    }
    return received;
}

//function to print snapshot
void print_snapshot(int64_t step, int32_t total, const snapshot &state)
{
    cout << "step " << step + 1 << " / " << total << endl;
    if(state.stackDepth == 0)
        cout << "call stack is empty." << endl;
    for(int p = 0; p < state.stackDepth; p++)
    {
        const frame &current = state.callStack[p];
        cout << current.functionName << (p == 0 ? " (top) " : "") << endl;
        for(int q = 0; q < current.argumentCount; q++)
            cout << "  argument " << current.argumentVector[q].variableName << " = " << current.argumentVector[q].variableValue << endl;
        for(int q = 0; q < current.localCount; q++)
            cout << "  local " << current.locals[q].variableName << " = " << current.locals[q].variableValue << endl;
    }
}

//function to show current
bool show_current(debugSession &session)
{
    if(!session.isActive)
    {
        cout << "start a session first." << endl;
        return false;
    }
    snapshot *current = read_snapshot_at(session.reader, session.currentStep);
    if(current == nullptr)
    {
        cout << "damaged .tdbg file\n";
        return false;
    }
    print_snapshot(session.currentStep, session.reader.timeTravelHeader.stepCount, *current);
    delete current;
    return true;
}

//function to move by
bool move_by(debugSession &session, int64_t amount)
{
    if(!session.isActive)
        return show_current(session);
    int64_t target = session.currentStep + amount;
    if(target < 0 || target >= session.reader.timeTravelHeader.stepCount)
    {
        cout << "requested step is outside the timeline." << endl;
        return false;
    }
    int64_t previous = session.currentStep;
    session.currentStep = target;
    if(show_current(session))
        return true;
    session.currentStep = previous;
    return false;
}

//function to command start
bool command_start(debugSession &session, const string &sourcePath)
{
    char temporary[] = "ttdb_last.tdbg.part.XXXXXX";
    int descriptor = mkstemp(temporary);
    if(descriptor < 0)
    {
        cout << "cannot create temporary download" << endl;
        return false;
    }
    close(descriptor);
    string error;
    if(!request_debug_file(sourcePath, temporary, error))
    {
        remove(temporary);
        cout << error << endl;
        return false;
    }
    timeTravelDebuggerReader candidate = {};
    if(!open_time_travel_debugger(temporary, candidate))
    {
        remove(temporary);
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }
    if(rename(temporary, "ttdb_last.tdbg") != 0)
    {
        close_time_travel_debugger(candidate);
        remove(temporary);
        cout << "cannot replace session file" << endl;
        return false;
    }
    close_time_travel_debugger(session.reader);
    session.reader = candidate;
    session.currentStep = 0;
    session.isActive = true;
    return show_current(session);
}

//function to print help
void print_help()
{
    cout << "start <file>" << endl;
    cout << "next" << endl;
    cout << "prv" << endl;
    cout << "jmp next <n>" << endl;
    cout << "jmp prv <n>" << endl;
    cout << "help" << endl;
    cout << "quit" << endl;
}

//function to run command
void run_command(debugSession &session, const string &line, bool &quit)
{
    istringstream input(line);
    string command, extra;
    if(!(input >> command))
        return;
    if(command == "ttdb")
        input >> command;
    if(command == "start")
    {
        string path;
        getline(input >> ws, path);
        size_t last = path.find_last_not_of(" \t\r");
        if(last != string::npos)
        {
            path.erase(last + 1);
            command_start(session, path);
            return;
        }
    }
    else if(command == "jmp")
    {
        string direction;
        int64_t amount;
        if((input >> direction >> amount) && !(input >> extra) && amount > 0 && amount <= 1000000000 && (direction == "next" || direction == "prv"))
        {
            move_by(session, direction == "next" ? amount : -amount);
            return;
        }
    }
    else if(!(input >> extra))
    {
        if(command == "next")
        {
            move_by(session, 1);
            return;
        }
        if(command == "prv")
        {
            move_by(session, -1);
            return;
        }
        if(command == "help")
        {
            print_help();
            return;
        }
        if(command == "quit" || command == "exit")
        {
            quit = true;
            return;
        }
    }
    cout << "unknown command" << endl;
    print_help();
}

//main function
int main(int argumentCount, char *argumentVector[])
{
    if(argumentCount != 3 || read_port(argumentVector[2]) < 0)
    {
        cerr << "usage : ./client <lb_ip> <lb_port>\n";
        return 1;
    }
    setup_signals();
    loadBalancerHost = argumentVector[1];
    loadBalancerPort = read_port(argumentVector[2]);
    debugSession session = {};
    bool isQuit = false;
    string line;
    print_help();
    while(flag && !isQuit)
    {
        cout << "ttdb> " << flush;
        if(!getline(cin, line))
            break;
        try
        {
            run_command(session, line, isQuit);
        }
        catch(const exception &error)
        {
            cout << error.what() << endl;
        }
    }
    close_time_travel_debugger(session.reader);
    return 0;
}
