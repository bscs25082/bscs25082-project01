#include "helper.h"
#include <thread>
#include <mutex>
#include <stdexcept>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

//global variables and consts
const int maximumPendingClients = 8;
string serverHost;
int serverPort;

//client information
struct clientInformation
{
    int socket;
    string clientAddress;
    int clientPort;
    time_t arrivedAt;
};

//queue class
template <typename temp>
class Queue
{
    struct Node
    {
        temp dataNode;
        Node *nextNode;
    };
    Node *frontNode;
    Node *rearNode;
    int count;

    public :
    //default contructor
    Queue()
    {
        frontNode = rearNode = nullptr;
        count = 0;
    }

    //delete copy and assignment constructor
    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;

    //destructor
    ~Queue()
    {
        while(frontNode != nullptr)
        {
            Node *tempNode = frontNode;
            frontNode = frontNode->nextNode;
            delete tempNode;
        }
    }
    
    //function to enqueue
    void enqueue(const temp &value)
    {
        Node *node = new Node{value, nullptr};
        if(rearNode == nullptr)
            frontNode = node;
        else
            rearNode->nextNode = node;
        rearNode = node;
        count++;
    }
    
    //function to deqeue
    temp dequeue()
    {
        if(is_empty())
            throw underflow_error("queue is empty");
        Node *tempNode = frontNode;
        temp value = tempNode->dataNode;
        frontNode = frontNode->nextNode;
        if(frontNode == nullptr)
            rearNode = nullptr;
        delete tempNode;
        count--;
        return value;
    }
    
    //function to check if its empty or not
    bool is_empty()
    {
        return count == 0;
    }

    //function to return size
    int size()
    {
        return count;
    }
};

//queue for pending clients
Queue<clientInformation> pendingClients;
mutex queueMutex;

//function to connect to server with wake up
int connect_to_server_with_wake_up()
{
    int socket = connect_to_host(serverHost.c_str(), serverPort, 100);
    if(socket >= 0)
        return socket;
    if(serverHost != "127.0.0.1" && serverHost != "localhost")
        return -1;
    string command = "./server " + to_string(serverPort) + " &";
    if(system(command.c_str()) != 0)
        return -1;
    for(int p = 0; p < 50 && flag; p++)
    {
        usleep(100000);
        socket = connect_to_host(serverHost.c_str(), serverPort, 100);
        if(socket >= 0)
            return socket;
    }
    return -1;
}

//function to relay to one client
void relay_one_client(clientInformation &client)
{
    int backend = connect_to_server_with_wake_up();
    if(backend < 0)
    {
        send_error(client.socket, "backend server failed to start");
        return;
    }
    bool isStarted = false;
    bool isValid = relay_stream(client.socket, backend, isStarted, 5000);
    shutdown(backend, SHUT_WR);
    if(isValid == false)
        send_error(client.socket, "upload checksum mismatch or incomplete upload");
    else
    {
        isStarted = false;
        isValid = relay_stream(backend, client.socket, isStarted, 30000);
        if((isValid == false) && (isStarted == false))
            send_error(client.socket, "response corrupted");
        shutdown(client.socket, SHUT_WR);
    }
    close(backend);
}

//function to accept the loop
void accept_loop(int listener)
{
    while(flag)
    {
        if(!wait_socket(listener, POLLIN, 200))
            continue;
        struct sockaddr_in address = {};
        socklen_t length = sizeof(address);
        int socket = accept(listener, (struct sockaddr *)&address, &length);
        if(socket < 0)
        {
            if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                perror("accept");
            continue;
        }
        if(!configure_socket(socket))
        {
            close(socket);
            continue;
        }
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
        clientInformation client = {socket, text, ntohs(address.sin_port), time(nullptr)};
        bool isAdded = false;
        try
        {
            lock_guard<mutex> guard(queueMutex);
            if(pendingClients.size() < maximumPendingClients)
            {
                pendingClients.enqueue(client);
                isAdded = true;
            }
        }
        catch(const exception &error)
        {
            cerr << error.what() << endl;
        }
        if(!isAdded)
        {
            send_error(socket, "server busy, try again later");
            close(socket);
        }
    }
}

//dispatcer loop function
void dispatcherLoop()
{
    while(flag)
    {
        clientInformation client;
        bool isFound = false;
        {
            lock_guard<mutex> guard(queueMutex);
            if(!pendingClients.is_empty())
            {
                client = pendingClients.dequeue();
                isFound = true;
            }
        }
        if(isFound == false)
        {
            usleep(10000);
            continue;
        }
        if(difftime(time(nullptr), client.arrivedAt) < 30)
            relay_one_client(client);
        close(client.socket);
    }
}

int main(int argumentCount, char *argumentVector[])
{
    if(argumentCount != 3 || read_port(argumentVector[2]) < 0)
    {
        cerr << "usage : ./load_balancer <server_ip> <server_port>" << endl;
        return 1;
    }
    setup_signals();
    serverHost = argumentVector[1];
    serverPort = read_port(argumentVector[2]);
    if((serverHost == "127.0.0.1" || serverHost == "localhost") &&
       serverPort == 5000)
    {
        cerr << "backend must use a different port from the load balancer." << endl;
        return 1;
    }
    int listener = create_listen_socket(6000);
    if(listener < 0)
    {
        perror("load balancer");
        return 1;
    }
    cout << "load balancer listening on port 5000" << endl;
    thread acceptThread(accept_loop, listener);
    dispatcherLoop();
    flag = 0;
    acceptThread.join();
    close(listener);
    while(!pendingClients.is_empty())
        close(pendingClients.dequeue().socket);
    return 0;
}
