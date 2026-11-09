#include "helper.h"
#include <cstdlib>
#include <stdexcept>
#include <poll.h>
#include <sys/socket.h>

//funnction to serve a client
void serve_client(int socket)
{
    char directory[] = "/tmp/ttdb-XXXXXX";
    if(mkdtemp(directory) == nullptr)
    {
        send_error(socket, "cannot create temporary directory");
        return;
    }
    string sourceDirectory = string(directory) + "/source.bin";
    string resolvedDirectory = string(directory) + "/resolve.bin";
    string sessionDirectory = string(directory) + "/session.tdbg";
    FILE *outputFile = nullptr;
    bool replyStarted = false;
    try
    {
        outputFile = fopen(sourceDirectory.c_str(), "wb");
        if(outputFile == nullptr)
            throw runtime_error("cannot save upload");
        string error;
        int status;
        bool received = receive_message(socket, outputFile, status, error, maximumSourceBytes, 5000);
        int closed = fclose(outputFile);
        outputFile = nullptr;
        if(!received)
        {
            if(error == "checksum mismatch, response corrupted")
                error = "upload checksum mismatch";
            if(error.empty())
                error = "invalid upload";
            throw runtime_error(error);
        }
        if(closed != 0)
            throw runtime_error("cannot save upload");
        create_session(sourceDirectory.c_str(), resolvedDirectory.c_str(), sessionDirectory.c_str());
        if(!send_file(socket, sessionDirectory.c_str(), replyStarted))
            throw runtime_error("cannot send session");
    }
    catch(const exception &error)
    {
        if(outputFile != nullptr)
            fclose(outputFile);
        if(!replyStarted && flag)
            send_error(socket, error.what());
    }
    remove(sourceDirectory.c_str());
    remove(resolvedDirectory.c_str());
    remove(sessionDirectory.c_str());
    rmdir(directory);
}

//main function
int main(int argumentCount, char *argumentVector[])
{
    setup_signals();
    if(argumentCount == 1)
    {
        try
        {
            int count = create_session("source.bin", "resolve.bin", "session.tdbg");
            cout << "created session.tdbg with " << count << " snapshots." << endl;
            return 0;
        }
        catch(const exception &error)
        {
            cerr << error.what() << '\n';
            return 1;
        }
    }
    if(argumentCount != 2 || read_port(argumentVector[1]) < 0)
    {
        cerr << "usage : ./server <port>" << endl;
        return 1;
    }
    int listener = create_listen_socket(read_port(argumentVector[1]));
    if(listener < 0)
    {
        perror("server");
        return 1;
    }
    cout << "server listening on port " << argumentVector[1] << endl;
    while(flag)
    {
        if(!wait_socket(listener, POLLIN, 200))
            continue;
        int client = accept(listener, nullptr, nullptr);
        if(client < 0)
            continue;
        if(configure_socket(client))
            serve_client(client);
        close(client);
    }
    close(listener);
    return 0;
}
