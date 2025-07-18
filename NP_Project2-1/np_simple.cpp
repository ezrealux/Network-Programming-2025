#include <iostream>
#include <sstream>
#include <string.h>

#include <vector>
#include <unordered_map>
#include <array>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>  // 定義 sockaddr_in

using namespace std;

#define MAX_USER_ID 30
#define BUFFER_SIZE 15000
#define DEFAULT_USER_NAME "(no name)"

const char kWelcome_Message[] =
    "****************************************\n"
    "** Welcome to the information server. **\n"
    "****************************************\n";
const char kBash[] = "% ";
int null_fd;

struct Command {
    vector<string> argv_vec;
    array<int, 2> pipe = {STDIN_FILENO, STDOUT_FILENO}; //上一個指令傳來的pipe
    int fd_out = STDOUT_FILENO;
    int fd_err = STDERR_FILENO;

    char** Vec2Char() {
        // vector<string>& argv_vec => char* argv_vec[] (C-style 陣列)
        char** argv = new char*[argv_vec.size() + 1];
        for (size_t i = 0; i < argv_vec.size(); i++) {
            argv[i] = strdup(argv_vec[i].c_str());
        }
        argv[argv_vec.size()] = nullptr; // execvp 需要 nullptr 結尾

        return argv;
    }
};

class NpShell {
    public:
    NpShell(bool debug) { DEBUG = debug; } 
    bool run();
    
    private:
    bool DEBUG;
    unordered_map<int, array<int, 2>> pipeMap;
    void SetStdinFrom(Command &command);
    void SetStdoutTo(Command &command, string arg, stringstream &ss);
    void ExecuteCommand(Command command);
    void UpdatePipemap();
    void CommandHandling(Command &command, string &arg, stringstream &ss);
};

class Server {
    public:
    Server(bool DEBUG, int qlen, int port);
    void accepting();
    
    private:
        bool DEBUG;

        struct sockaddr_in fsin; // the address of a client
        socklen_t alen; // length of client's address
        int msock; // master server socket
        int ssock; // slave server socket
        struct sockaddr_in sin; // an Internet endpoint address
};

void reaper(int sig) { while (waitpid(-1, NULL, WNOHANG) > 0); }

int main(int argc, char* argv[]) { 
    signal(SIGCHLD, reaper);
    setenv("PATH", "bin:.", 1);
    null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

    int port;
    if (argc == 2) port = stol(argv[1]);
    else {
        cout << "Port!!!"<<endl;
        return 1;
    }
    Server server(false, 5, port);
    server.accepting();
    
    return 0;
}

bool NpShell::run() {
    signal(SIGCHLD, SIG_IGN);
    setenv("PATH", "bin:.", 1);
    //char buffer[BUFFER_SIZE];
    
    while (true) {
        cout << "% " << flush;//輸入一行prompt

        // 因為socket不同於在本機上執行shell，基於協定會額外收到一個'\r'，需特別處理
        string input;
        char buffer[BUFFER_SIZE] = {0};
        ssize_t bytesRead = read(STDIN_FILENO, buffer, BUFFER_SIZE - 1);
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            for (int i = 0; i < bytesRead; i++) {// 處理 '\r' 與 '\n'
                if (buffer[i] == '\r') continue;
                if (buffer[i] == '\n') break;
                input.push_back(buffer[i]);
            }
        }
        if (input.empty()) continue; // Get user input, handle empty input
        if (input == "exit") break;

        string arg;
        stringstream ss(input);
        Command command;
        while (getline(ss, arg, ' ')) { // 以空格為分隔符讀取
            if (arg[0] == '|' || arg[0] == '!' || arg[0] == '>') { // 遇到|, !, >，輸出 args
                CommandHandling(command, arg, ss);
                continue;
            }
            command.argv_vec.push_back(arg);
        }
        arg.clear();

        if (!command.argv_vec.empty()) { CommandHandling(command, arg, ss); }
    }
    return 0;
}

void NpShell::SetStdinFrom(Command &command) {
    if (pipeMap.count(0)) { // pipe fd_in
        command.pipe = pipeMap[0];
        pipeMap.erase(0);
    } else { command.pipe = {STDIN_FILENO, STDOUT_FILENO}; }
}

void NpShell::SetStdoutTo(Command &command, string arg, stringstream &ss) {
    if (arg.empty()) {
        command.fd_out = STDOUT_FILENO;
        command.fd_err = STDERR_FILENO;
        return;
    }

    array<int, 2> pipe_tmp;
    int pipeNum = 0;
    if (arg.size() > 1) { pipeNum = stoi(arg.substr(1)); }
    
    if (arg[0] == '>') { // '>'
        string filename;
        getline(ss, filename, ' ');
        if ((command.fd_out = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664)) == -1) {
            cerr << "failed to open file: " << strerror(errno) << endl;
            exit(0); 
        }
        if (DEBUG) { cerr<<command.pipe[0]<< " -> \""<<command.argv_vec[0]<<"\" -> "<<command.fd_out<<" -> \""<<filename<< "\"\n"; }
    } else { // '|' or '!'
        if (!pipeMap.count(pipeNum)) { //create pipe
            while (pipe(pipe_tmp.data()) == -1) {
                if (errno == EMFILE || errno == ENFILE) { wait(nullptr); } // 如沒有pipe則等wait，等其他process釋放資源
            }
            fcntl(pipe_tmp[0], F_SETFD, FD_CLOEXEC);
            fcntl(pipe_tmp[1], F_SETFD, FD_CLOEXEC);
            pipeMap.emplace(pipeNum, pipe_tmp);
        }

        if (arg[0] == '|') {
            command.fd_out = pipeMap[pipeNum][1];
            if (DEBUG) { cerr<<command.pipe[0]<< " -> \""<<command.argv_vec[0]<<"\" -> "<<command.fd_out<<" -> "<<pipeMap[pipeNum][0]<<"\n"; }
        } else {
            command.fd_out = pipeMap[pipeNum][1];
            command.fd_err = pipeMap[pipeNum][1];
        }
    }
}

void NpShell::ExecuteCommand(Command command) {
    if (command.argv_vec.empty()) return; // 如果沒有指令，直接返回
    char** argv = command.Vec2Char();
    
    // Bulit-in commands
    if (!strcmp(argv[0], "exit")) { return; }
    if (!strcmp(argv[0], "setenv")) {
        setenv(argv[1], argv[2], 1);
        return;
    }
    if (!strcmp(argv[0], "printenv")) {
        if (const char *env = getenv(argv[1])) { cout << env << endl << flush; }
        return;
    }

    pid_t pid;
    while ((pid = fork()) == -1) { // 創建child process，如失敗(process滿了)則wait，等其他child釋放process資源
        if (errno == EAGAIN) { wait(nullptr); }
    }
    if (pid) { // parent process
        struct stat fd_stat;
        fstat(command.fd_out, &fd_stat);
        if (command.pipe[0] != STDIN_FILENO) { // pipe只在用完後回收
            close(command.pipe[0]); //cout << "parent close: "<<command.pipe[0]<<endl;
            close(command.pipe[1]); //cout << "parent close: "<<command.pipe[1]<<endl;
        }
        if (command.fd_out != STDOUT_FILENO && S_ISREG(fd_stat.st_mode)) { // 如果是通向file則須主動關閉out
            close(command.fd_out); //cout << "parent close: "<<command.fd_out<<endl;
        }

        if (!S_ISFIFO(fd_stat.st_mode)) { waitpid(pid, nullptr, 0); }
        return;

    } else { //child process
        dup2(command.pipe[0], STDIN_FILENO); //command.pipe[0]取代STDIN_FILENO
        dup2(command.fd_out, STDOUT_FILENO); //command.fd_out取代STDOUT_FILENO
        dup2(command.fd_err, STDERR_FILENO); //command.fd_err取代STDERR_FILENO

        if (execvp(argv[0], argv) < 0 && errno == ENOENT) {
            cerr << "Unknown command: [" << argv[0] << "]." << endl;
            exit(127); // **標準錯誤碼**
        }
        exit(0);
    }
    return;
}

void NpShell::UpdatePipemap() { // PipeMap的key值全部-1
    unordered_map<int, array<int, 2>> new_map;
    for (const auto[key, value] : pipeMap) { new_map.emplace(key - 1, value); } // reduce pipeNum
    pipeMap = move(new_map);
}

void NpShell::CommandHandling(Command &command, string &arg, stringstream &ss) {
    SetStdinFrom(command);
    SetStdoutTo(command, arg, ss);
    if (!command.argv_vec.empty()) ExecuteCommand(command);
    command.argv_vec.clear();  // 清空 vector
    if (arg != "|") UpdatePipemap();
}

Server::Server(bool DEBUG, int qlen, int port) { 
    //qlen; (maximum length of the server request queue)
    bzero((char *)&sin, sizeof(sin)); 
    sin.sin_family = AF_INET; 
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port); 

    int enable = 1;
    // Allocate a socket, set socket option, bind the socket, listen to the socket; 
    if ((msock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0 /*Stands for tcp*/)) < 0) { cerr<<"NO SOCKET"<<endl; exit(3); }
    if (setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {cerr<<"NO setsockopt"<<endl; exit(4);}
    if (bind(msock, (struct sockaddr *)&sin, sizeof(sin)) < 0) { close(msock); cerr<<"NO BIND"<<endl; exit(5); }
    if (listen(msock, qlen) < 0) { close(msock); cerr<<"NO LISTEN"<<endl; exit(6); }
}

void Server::accepting() {
    while (1) { 
        alen = sizeof(fsin); 
        if ((ssock = accept(msock, (struct sockaddr *) &fsin, &alen)) < 0) { 
            if (errno == EINTR) continue; 
            exit(7);
        }

        NpShell npshell(false);
        switch (fork()) { 
        case -1:
            exit(8);
        case 0: // child
            (void) close(msock);
            dup2(ssock, STDIN_FILENO);
            dup2(ssock, STDOUT_FILENO);
            dup2(ssock, STDERR_FILENO);
            exit(npshell.run());
        default: // parent
            (void) close(ssock);
            break;
        }
    }
}