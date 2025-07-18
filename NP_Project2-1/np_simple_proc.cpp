#include <iostream>
#include <sstream>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <map>
#include <unordered_map>
#include <utility>
#include <array>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>  // 定義 sockaddr_in
#include <arpa/inet.h>

using namespace std;

#define MAX_USER_ID 30
#define BUFFER_SIZE 15000
#define DEFAULT_USER_NAME "(no name)"
const char kWelcome_Message[] =
    "****************************************\n"
    "** Welcome to the information server. **\n"
    "****************************************\n";
const char kBash[] = "% ";
int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

struct Command {
    vector<string> argv_vec;
    array<int, 2> pipe = {STDIN_FILENO, STDOUT_FILENO}; //上一個指令傳來的pipe
    int fd_out = STDOUT_FILENO;
    int fd_err = STDERR_FILENO;

    char** Vec2Char() { // vector<string>& argv_vec => char* argv_vec[] (C-style 陣列)
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
    NpShell(int id, int fd);
    ~NpShell();
    bool run();
    int ID;
    int FD;
    string username;
    string IpPort;
    
    private:
    unordered_map<int, array<int, 2>> pipeMap;
    unordered_map<string, string> ENV_VAR;
    string STDIN_MESSAGE;
    string STDOUT_MESSAGE;

    int SetStdinFrom(Command &command, string input, string arg);
    int SetStdoutTo(Command &command, string input, string arg, stringstream &ss);
    void ExecuteCommand(Command command, int stdin_stat, int stdout_stat);
    void BuiltInCommand(Command command, char* argv[]);
    void UpdatePipemap();
};

class Server {
    public:
    Server(int port);
    ~Server();
    void accepting();
    
    private:
        int msock; // master server socket
        struct sockaddr_in sin; // an Internet endpoint address
        
        fd_set read_fds;
        int max_fd_num;

        void HandleConnection();
        void HandleDisconnection(int fd);
};

struct UserSpace {
    map<int, NpShell*> idx_by_id;
    unordered_map<int, NpShell*> idx_by_fd;
    set<int> ids_available;
    set<string> names;
    map<pair<int, int>, array<int, 2>> user_pipe;
    bool block[MAX_USER_ID+1][MAX_USER_ID+1] = {false};
    
    void insert(int id, int fd, NpShell* shell_ptr) {
        idx_by_id[id] = idx_by_fd[fd] = shell_ptr;
        ids_available.erase(id);
    }
    void remove(int fd) {
        NpShell* exit_user = idx_by_fd[fd];
        idx_by_id.erase(exit_user->ID);
        idx_by_fd.erase(exit_user->FD);
        ids_available.insert(exit_user->ID);
        names.erase(exit_user->username);
        for (int i=0; i<=MAX_USER_ID; i++) {
            block[exit_user->ID][i] = false;
            block[i][exit_user->ID] = false;
        }

        for (auto it = user_pipe.begin(); it != user_pipe.end(); ) {
            auto const[key, value] = *it;
            auto[receiver, sender] = key;
            if (receiver == exit_user->ID || sender == exit_user->ID) {
                cout << "[Server] \tparent close: "<<value[0]<<endl;
                cout << "[Server] \tparent close: "<<value[1]<<endl;
                close(value[0]); // 先關閉 pipe
                close(value[1]);
                it = user_pipe.erase(it); // 刪除並移到下一個
            } else { ++it; } // 正常情況下才 ++
        }
        delete exit_user;
    }
    
    void Broadcast(string message, int sender_id=-1) {
        for (const auto &[key, shell_ptr] : idx_by_id) {
            if (sender_id > 0) {
                if (block[sender_id][key] == true) continue;
            }
            dprintf(shell_ptr->FD, "%s", message.c_str()); 
        }
    }
} ActiveUsers;

NpShell::NpShell(int id, int fd) { 
    ID = id;
    FD = fd;
    ENV_VAR["PATH"] = "bin:.";
    username = DEFAULT_USER_NAME;
    
    sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);
    getpeername(FD, (sockaddr *) &src_addr, &src_len); //tcp

    stringstream ss;
    ss << inet_ntoa(src_addr.sin_addr) << ":" << ntohs(src_addr.sin_port);
    IpPort = ss.str();
}

NpShell::~NpShell() {
    for (const auto &[key, value] : pipeMap) {
        close(value[0]);
        close(value[1]);
    }
}

bool NpShell::run() {
    // 因為socket不同於在本機上執行shell，基於協定會額外收到一個'\r'，需特別處理
    string input;
    char recv_buf[1];
    while (recv(FD, recv_buf, 1, 0)) {
        if (recv_buf[0] == '\r') { continue; }
        if (recv_buf[0] == '\n') { break; }
        input.append(recv_buf, 1);
    }
    
    if (input == "exit") {
        dprintf(FD, "%s", kBash);
        return 1;
    }
    cout << "[Server] Client "<<IpPort<<" run the prompt, id="<<ID<<", fd="<<FD<< endl;
    cout << "[Server] \tprompt: \"" << input <<"\", length="<<input.size()<<endl;

    string arg;
    stringstream ss(input);
    Command command;
    int stdin_stat = 0, stdout_stat = 0;
    while (getline(ss, arg, ' ')) { // 以空格為分隔符讀取
        if (arg[0] == '|' || arg[0] == '!' || arg[0] == '>' || arg[0] == '<') {
            if ((stdin_stat = SetStdinFrom(command, input, arg))) { // if pipe is from user_pipe
                if (!getline(ss, arg, ' ')) { arg.clear(); }// get next arg for output
            }
            stdout_stat = SetStdoutTo(command, input, arg, ss); // is user pipe, may need to read stdin again
            
            streampos position = ss.tellg(); // 紀錄存檔點用來倒回
            if (!getline(ss, arg, ' ')) { arg.clear(); }// get next arg for output
            if (arg[0] == '<') { stdin_stat = SetStdinFrom(command, input, arg); } // 確定要做user pipe再改command.pipe
            else { ss.seekg(position); } // 如果不是user pipe，返回存檔點，就當作沒讀過
            if (stdout_stat != 4) { UpdatePipemap(); } // "|" piping without updating pipeMap

            ExecuteCommand(command, stdin_stat, stdout_stat);
            command.argv_vec.clear();
            continue;
        }
        command.argv_vec.push_back(arg);
        
        if (arg == "yell" || arg == "tell" || arg == "block") { // 如指令為yell或tell，後面整句都是arg
            if (arg == "tell") { // 如果是tell，要再拿出寄出對象的arg
                getline(ss, arg, ' ');
                command.argv_vec.emplace_back(arg);
            }
            getline(ss, arg);
            command.argv_vec.emplace_back(arg);
        }
    }
    arg.clear();
    if (!command.argv_vec.empty()) { // no user pipe, no file pipe, no number pipe, no ordinary pipe
        stdin_stat = SetStdinFrom(command, input, arg);
        stdout_stat = SetStdoutTo(command, input, arg, ss);
        cout << "[Server] STDIN_FLAG="<<stdin_stat<<" STDOUT_FLAG="<<stdout_stat<<endl;

        UpdatePipemap(); 
        ExecuteCommand(command, stdin_stat, stdout_stat);
        command.argv_vec.clear();  // 清空 vector
    }

    dprintf(FD, "%s",kBash);
    return 0;
}

int NpShell::SetStdinFrom(Command &command, string input, string arg) { // 0: not user pipe, -1: set error, 1: set success
    command.pipe = {FD, FD};
    
    if (pipeMap.count(0)) { // pipe fd_in
        command.pipe = pipeMap[0];
        pipeMap.erase(0); // if pipeMap[0]有pipe，則需讀取pipe並刪除
    }

    if (arg[0] == '<') { // if is user pipe，則覆蓋掉其他pipe
        command.pipe[0] = command.pipe[1] = null_fd;
        int sender_id = stoi(arg.substr(1));
        if (!ActiveUsers.idx_by_id.count(sender_id)) { // sender user doesn't exist
            STDIN_MESSAGE = "*** Error: user #" + to_string(sender_id) + " does not exist yet. ***\n";
            return -1;
        }

        pair<int, int> userpipe_idx(sender_id, ID);
        if (!ActiveUsers.user_pipe.count(userpipe_idx)) { // user pipe doesn't exist
            STDIN_MESSAGE = "*** Error: the pipe #" + to_string(sender_id) + "->#" + to_string(ID) + " does not exist yet. ***\n";
            return -1;
        }

        command.pipe = ActiveUsers.user_pipe[userpipe_idx];
        ActiveUsers.user_pipe.erase(userpipe_idx);
        
        STDIN_MESSAGE = "*** " + username + " (#" + to_string(ID) + ") just received from " + 
                        ActiveUsers.idx_by_id[sender_id]->username + " (#" + to_string(sender_id) + ") by '" + input + "' ***\n";
        
        return 1;
    }
    return 0;
}

int NpShell::SetStdoutTo(Command &command, string input, string arg, stringstream &ss) {
    command.fd_out = command.fd_err = FD;
    
    if (arg[0] == '>') { // '>'
        if (arg.size() > 1) { // user pipe
            command.fd_out = null_fd;
            int receiver_id = stoi(arg.substr(1));
            if (!ActiveUsers.idx_by_id.count(receiver_id)) { // receiver user doesn't exist
                STDOUT_MESSAGE = "*** Error: user #" + to_string(receiver_id) + " does not exist yet. ***\n";
                return -1;
            }

            pair<int, int> userpipe_idx(ID, receiver_id);
            if (ActiveUsers.user_pipe.count(userpipe_idx)) { //user pipe already exists
                STDOUT_MESSAGE = "*** Error: the pipe #" + to_string(ID) + "->#" + to_string(receiver_id) + " already exists. ***\n";
                return -1;
            }

            array<int, 2> pipe_tmp;
            while (pipe(pipe_tmp.data()) == -1) { // create pipe
                if (errno == EMFILE || errno == ENFILE) { wait(nullptr); } // 如沒有pipe則等wait，等其他process釋放資源
            }
            fcntl(pipe_tmp[0], F_SETFD, FD_CLOEXEC);
            fcntl(pipe_tmp[1], F_SETFD, FD_CLOEXEC);
            ActiveUsers.user_pipe.emplace(userpipe_idx, pipe_tmp);
            command.fd_out = ActiveUsers.user_pipe[userpipe_idx][1];

            STDOUT_MESSAGE = "*** " + username + " (#" + to_string(ID) + ") just piped \'" + input + "\' to " + 
                             ActiveUsers.idx_by_id[receiver_id]->username + " (#" + arg.substr(1) + ") ***\n";

            return 1; // user pipe
        } else { // pipe to file
            string filename;
            getline(ss, filename, ' ');
            if ((command.fd_out = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664)) == -1) {
                cout<<"[Server] \tfailed to open file: " << strerror(errno) << endl;
                return -1; 
            }

            return 2; // file pipe
        }
    } else if (arg[0] == '|' || arg[0] == '!') { // '|' or '!'
        int pipeNum = 0;
        if (arg.size() > 1) { pipeNum = stoi(arg.substr(1)); } //number pipe

        if (!pipeMap.count(pipeNum)) { //create pipe
            array<int, 2> pipe_tmp;
            while (pipe(pipe_tmp.data()) == -1) {
                if (errno == EMFILE || errno == ENFILE) { wait(nullptr); } // 如沒有pipe則等wait，等其他process釋放資源
            }
            fcntl(pipe_tmp[0], F_SETFD, FD_CLOEXEC);
            fcntl(pipe_tmp[1], F_SETFD, FD_CLOEXEC);
            pipeMap.emplace(pipeNum, pipe_tmp);
        }

        command.fd_out = pipeMap[pipeNum][1];
        if (arg[0] == '!') { command.fd_err = pipeMap[pipeNum][1]; }

        if (pipeNum) return 3; // number pipe
        return 4; // ordinary pipe
    }

    return 0;
}

void NpShell::ExecuteCommand(Command command, int stdin_stat, int stdout_stat) {
    cout<<"[Server] \t("<<((stdin_stat<0)?"null_fd":to_string(command.pipe[1]))<<"->"<<((stdin_stat<0)?"null_fd":to_string(command.pipe[0]))<<")->"
        <<command.argv_vec[0]<<"->"<<((stdout_stat<0)?"null_fd":to_string(command.fd_out))<<endl;
    cout<<"[Server] \tSTDIN_FLAG="<<stdin_stat<<" STDOUT_FLAG="<<stdout_stat<<endl;
    
    if (stdin_stat == -1) { dprintf(FD, "%s", STDIN_MESSAGE.c_str()); }
    else if (stdin_stat == 1) { ActiveUsers.Broadcast(STDIN_MESSAGE); }
    if (stdout_stat == -1) { dprintf(FD, "%s", STDOUT_MESSAGE.c_str()); }
    else if (stdout_stat == 1) { ActiveUsers.Broadcast(STDOUT_MESSAGE); }

    if (command.argv_vec.empty()) return; // 如果沒有指令，直接返回
    char** argv = command.Vec2Char();
    
    if (!strcmp(argv[0], "setenv") || !strcmp(argv[0], "printenv") || !strcmp(argv[0], "who") || !strcmp(argv[0], "yell") || !strcmp(argv[0], "tell") || !strcmp(argv[0], "name") || !strcmp(argv[0], "block")) {
        BuiltInCommand(command, argv);
    } else {
        pid_t pid;
        while ((pid = fork()) == -1) { // 創建child process，如失敗(process滿了)則wait，等其他child釋放process資源
            if (errno == EAGAIN) { wait(nullptr); }
        }
        if (pid) { // parent process
            struct stat fd_stat;
            fstat(command.fd_out, &fd_stat);
            if (command.pipe[0] != FD && command.pipe[0] != null_fd) { // pipe只在用完後回收
                close(command.pipe[0]); 
                cout << "[Server] \tparent close: "<<command.pipe[1]<<endl;
            }
            if (command.pipe[1] != FD && command.pipe[1] != null_fd) { // pipe只在用完後回收
                close(command.pipe[1]);
                cout << "[Server] \tparent close: "<<command.pipe[0]<<endl;
            }
            if (command.fd_out != FD && S_ISREG(fd_stat.st_mode)) { // 如果是通向file則須主動關閉out
                close(command.fd_out);
                cout << "[Server] \tparent close file: "<<command.fd_out<<endl;
            }
            if (!S_ISFIFO(fd_stat.st_mode)) { waitpid(pid, nullptr, 0); }
        } else { //child process
            for (const auto &[key, value] : ENV_VAR) { setenv(key.c_str(), value.c_str(), 1); }

            dup2(command.pipe[0], STDIN_FILENO); //command.pipe[0]取代STDIN_FILENO
            dup2(command.fd_out, STDOUT_FILENO); //command.fd_out取代STDOUT_FILENO
            dup2(command.fd_err, STDERR_FILENO); //command.fd_err取代STDERR_FILENO

            if (execvp(argv[0], argv) < 0 && errno == ENOENT) {
                cerr << "Unknown command: [" << argv[0] << "]." << endl;
                exit(127); // **標準錯誤碼**
            }
            exit(0);
        }
    }
    return;
}

void NpShell::BuiltInCommand(Command command, char* argv[]) {
    if (!strcmp(argv[0], "setenv")) {
        ENV_VAR[command.argv_vec[1]] = command.argv_vec[2];
    } else if (!strcmp(argv[0], "printenv")) {
        if (ENV_VAR.count(command.argv_vec[1])) { dprintf(command.fd_out, "%s\n", ENV_VAR[command.argv_vec[1]].c_str()); }
    
    } else if (!strcmp(argv[0], "who")) {
        stringstream output;
        output << "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
        for (const auto &[key, shell_ptr] : ActiveUsers.idx_by_id) {
            output << key << '\t' << shell_ptr->username << '\t' << shell_ptr->IpPort;
            if (key == ID) output << "\t<-me";
            output << '\n';
        }
        dprintf(command.fd_out, output.str().c_str(), output.str().size(), 0);
    } else if (!strcmp(argv[0], "tell")) {
        int recv_id = stoi(argv[1]);
        if (ActiveUsers.idx_by_id.count(recv_id)) {
            dprintf(ActiveUsers.idx_by_id[recv_id]->FD, "*** %s told you ***: %s\n", username.c_str(), argv[2]);
        } else {
            dprintf(command.fd_out, "*** Error: user #%d does not exist yet. ***\n", recv_id);
        }
    } else if (!strcmp(argv[0], "yell")) {
        string message = "*** " + username + " yelled ***: " + argv[1] + "\n";
        ActiveUsers.Broadcast(message, ID);
    } else if (!strcmp(argv[0], "name")) {
        if (ActiveUsers.names.count(argv[1])) { // new_name already exists
            dprintf(command.fd_out, "*** User \'%s\' already exists. ***\n", argv[1]);
        } else { // remove old name from names
            ActiveUsers.names.erase(username);
            username = argv[1];
            ActiveUsers.names.emplace(username);

            string message = "*** User from " + IpPort + " is named \'" + argv[1] + "\'. ***\n";
            ActiveUsers.Broadcast(message, ID);
        }
    } else if (!strcmp(argv[0], "block")) {
        int block_id = stoi(argv[1]);
        ActiveUsers.block[ID][block_id] = true;
        ActiveUsers.block[block_id][ID] = true;
    }
}

void NpShell::UpdatePipemap() { // PipeMap的key值全部-1
    unordered_map<int, array<int, 2>> new_map;
    for (const auto[key, value] : pipeMap) { new_map.emplace(key - 1, value); } // reduce pipeNum
    pipeMap = move(new_map);
}

Server::Server(int port) { 
    bzero((char *)&sin, sizeof(sin)); 
    sin.sin_family = AF_INET; 
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port); 

    int enable = 1;
    // Allocate a socket, set socket option, bind the socket, listen to the socket; 
    if ((msock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0 /*Stands for tcp*/)) < 0) { cerr<<"[Server]"<<"NO SOCKET"<<endl; exit(3); }
    if (setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {cerr<<"[Server]"<<"NO setsockopt"<<endl; exit(4);}
    if (bind(msock, (struct sockaddr *)&sin, sizeof(sin)) < 0) { close(msock); cerr<<"[Server]"<<"NO BIND"<<endl; exit(5); }
    if (listen(msock, 1) < 0) { close(msock); cerr<<"[Server]"<<"NO LISTEN"<<endl; exit(6); }

    FD_ZERO(&read_fds);
    FD_SET(msock, &read_fds);
    max_fd_num = msock;
    for (int i = 1; i <= MAX_USER_ID; i++) { ActiveUsers.ids_available.emplace(i); } //放入所有尚能使用的id
}

Server::~Server() {
    for (auto& [key, ptr] : ActiveUsers.idx_by_fd) { delete ptr; }
    close(msock);
}

void Server::HandleConnection() {
    // accept, create ssock
    int ssock;
    struct sockaddr fsin;
    socklen_t alen = sizeof(fsin);
    alen = sizeof(fsin); 
    if ((ssock = accept4(msock, (struct sockaddr *) &fsin, &alen, SOCK_NONBLOCK | SOCK_CLOEXEC)) < 0) { exit(7); }
    FD_SET(ssock, &read_fds);
    max_fd_num = max(max_fd_num, ssock);

    // take minimum of available id as a key to its npshell object
    int user_id = *ActiveUsers.ids_available.cbegin();
    NpShell* new_user = new NpShell(user_id, ssock);
    ActiveUsers.insert(user_id, ssock, new_user);

    dprintf(ssock, "%s", kWelcome_Message);
    string message = "*** User \'" + new_user->username + "\' entered from "+ new_user->IpPort + ". ***\n";
    ActiveUsers.Broadcast(message);
    cout << "[Server] Client " << new_user->IpPort << " connected using TCP, id="<<new_user->ID<<", fd="<<new_user->FD << endl;
    dprintf(ssock, "%s", kBash);
}

void Server::HandleDisconnection(int fd) {
    NpShell* exit_user = ActiveUsers.idx_by_fd[fd];
    string message = "*** User \'" + exit_user->username + "\' left. ***\n";
    cout <<"[Server] Client "<< exit_user->IpPort <<" exited, id="<< exit_user->ID <<", fd="<<fd<< endl;
    
    // remove info and return resource
    ActiveUsers.remove(fd);
    ActiveUsers.Broadcast(message);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    FD_CLR(fd, &read_fds);
}

void Server::accepting() {
    while (1) { 
        fd_set select_fd_set = read_fds;
        select(max_fd_num + 1, &select_fd_set, nullptr, nullptr, nullptr);

        for (int fd = 0; fd <= max_fd_num; fd++) {
            if (FD_ISSET(fd, &select_fd_set)) {
                if (fd == msock) { HandleConnection(); } 
                else {
                    if (ActiveUsers.idx_by_fd[fd]->run()) { HandleDisconnection(fd); }
                }
            }
        }
    }
}

int main(int argc, char* argv[]) { 
    signal(SIGCHLD, SIG_IGN);
    //setenv("PATH", "bin:.", 1);
    
    int port;
    if (argc == 2) port = stol(argv[1]);
    else return 1;
    
    Server server(port);
    server.accepting();
    
    return 0;
}