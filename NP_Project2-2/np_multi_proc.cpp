#include <iostream>
#include <sstream>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <unordered_map>
#include <array>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <semaphore.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <netinet/in.h>  // 定義 sockaddr_in
#include <arpa/inet.h>

using namespace std;

//----------------------------------------------------------------------------------------------------------------------
// OBJECTIVE
//----------------------------------------------------------------------------------------------------------------------

#define MAX_USER_ID 30
#define MAX_MSG_LEN 1024
#define BUFFER_SIZE 15000
#define SIGMSG SIGUSR1
#define SIGRECV SIGUSR2

const char DEFAULT_USER_NAME[] = "(no name)";
const char kWelcome_Message[] =
    "****************************************\n"
    "** Welcome to the information server. **\n"
    "****************************************\n";
const char kBash[] = "% ";
const string USER_PIPE_DIR = "./user_pipe/";
int null_fd;
int ID = 0;
void *addr;

struct Command {
    vector<string> argv_vec;
    string fifo_pipe_name;
    array<int, 2> Inpipe = {STDIN_FILENO, null_fd}; //上一個指令傳來的pipe
    //array<int, 2> Outpipe = {null_fd, STDOUT_FILENO};
    int fd_out = STDOUT_FILENO;
    int fd_err = STDERR_FILENO;
    int sender_id = 0, receiver_id = 0;

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

struct ClientInfo {
    bool IsValid = false;
    int pid;
    char username[MAX_MSG_LEN];
    char IpPort[MAX_MSG_LEN];

    static char* Fd2Port(int fd) {
        sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        getpeername(fd, (sockaddr *) &src_addr, &src_len); //tcp

        stringstream ss;
        ss << inet_ntoa(src_addr.sin_addr) << ":" << ntohs(src_addr.sin_port);
        string ip_str = ss.str();

        char* res = new char[ip_str.size() + 1];
        strcpy(res, ip_str.c_str());
        return res; // caller 要記得 delete[]
    }
};
struct MessageBox {
    char message[MAX_MSG_LEN];
    bool has_msg = false;
    sem_t lock;
};
struct SharedTable {
    ClientInfo clients[MAX_USER_ID+1];
    MessageBox msgBoxes[MAX_USER_ID+1];
    bool ids_available[MAX_USER_ID+1];
    bool userPipe[MAX_USER_ID+1][MAX_USER_ID+1]; // (sender_id, receiver_id)
    bool Block[MAX_USER_ID+1][MAX_USER_ID+1] = {false};
    sem_t ClientsLock;
    sem_t UserPipeLock;
};
SharedTable* sharedtable;

class NpShell {
    public:
    unordered_map<int, int> UserPipe; // (receiver_id): (fd)

    NpShell(bool debug) { DEBUG = debug; }
    void run();

    private:
    bool DEBUG;
    struct sigaction sa;
    int stdin_stat = 0, stdout_stat = 0;
    stringstream STDIN_MESSAGE, STDOUT_MESSAGE;
    unordered_map<int, array<int, 2>> NumberPipe; // number: (fd_out, fd_in)

    int SetStdinFrom(Command &command, string input, string arg);
    int SetStdoutTo(Command &command, string input, string arg, stringstream &ss);
    void OutputDEBUGMsg(Command command);
    void ExecuteCommand(Command command);
    void BuiltInCommand(Command command, char* argv[]);
    void UpdateNumberPipe();
    void ClearUserPipe();
} npshell(true);

class Server {
    public:
    Server(int qlen, int port);
    void accepting();
    
    private:
        int msock; // master server socket
        struct sockaddr_in sin; // an Internet endpoint address
};
Server* server;

//----------------------------------------------------------------------------------------------------------------------
// SIGNAL FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void reaper(int sig) { while(waitpid(-1, NULL, WNOHANG) > 0); } //zombie process回收

void SendMessage(string msg, int receiver_id = -1, bool block=false) {
    if (receiver_id == -1) { // == Broadcast ==
        for (int user_id = 1; user_id <= MAX_USER_ID; user_id++) {
            if (sharedtable->clients[user_id].IsValid == true) {
                if (block && sharedtable->Block[ID][user_id]) continue;
                sem_wait(&sharedtable->msgBoxes[user_id].lock);
                strcpy(sharedtable->msgBoxes[user_id].message, msg.c_str());
                sharedtable->msgBoxes[user_id].has_msg = true;
                if (kill(sharedtable->clients[user_id].pid, SIGMSG) < 0) {
                    perror("kill failed");
                    sem_post(&sharedtable->msgBoxes[user_id].lock); // 確保解鎖
                }
            }
        }
    } else { // == TELL ==
        if (block && sharedtable->Block[ID][receiver_id]) return;
        sem_wait(&sharedtable->msgBoxes[receiver_id].lock);
        strcpy(sharedtable->msgBoxes[receiver_id].message, msg.c_str());
        sharedtable->msgBoxes[receiver_id].has_msg = true;
        if (kill(sharedtable->clients[receiver_id].pid, SIGMSG) < 0) {
            perror("kill failed");
            sem_post(&sharedtable->msgBoxes[receiver_id].lock); // 確保解鎖
        }
    }
}
void SIGMSGHandler(int signo) {
    if (sharedtable->msgBoxes[ID].has_msg == true) {
        sharedtable->msgBoxes[ID].has_msg = false;
        if (write(STDOUT_FILENO, sharedtable->msgBoxes[ID].message, strlen(sharedtable->msgBoxes[ID].message)) < 0) {
            sem_post(&sharedtable->msgBoxes[ID].lock); 
            return;
        }
    }
    sem_post(&sharedtable->msgBoxes[ID].lock); 
}

void SIGRECVHandler(int sig, siginfo_t *si, void *unused) {
    sem_wait(&sharedtable->UserPipeLock);
    int receiver_id = si->si_value.sival_int;
    string FIFO_name = USER_PIPE_DIR + to_string(ID) + "_to_" + to_string(receiver_id);
    stringstream TerminalMsg;
    TerminalMsg << "[Server] #"<<ID<<" Parent close="<<npshell.UserPipe[receiver_id]<<", ";

    close(npshell.UserPipe[receiver_id]); 
    npshell.UserPipe.erase(receiver_id);
    unlink(FIFO_name.c_str()); 
    sharedtable->userPipe[ID][receiver_id] = false;
    TerminalMsg << "unlinked \""<<FIFO_name<<"\"\n";

    SendMessage(TerminalMsg.str(), 0);       
    sem_post(&sharedtable->UserPipeLock);
}

//----------------------------------------------------------------------------------------------------------------------
// Utility FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void HandleConnection(int ssock) {
    sem_wait(&sharedtable->ClientsLock);
    
    sharedtable->clients[ID].IsValid = true;
    strcpy(sharedtable->clients[ID].username, DEFAULT_USER_NAME);
    sharedtable->clients[ID].pid = getpid();
    strcpy(sharedtable->clients[ID].IpPort, ClientInfo::Fd2Port(ssock));

    sharedtable->ids_available[ID] = false; // take minimum of available id as a key to its npshell object

    dprintf(STDIN_FILENO, "%s", kWelcome_Message);
    stringstream message;
    message <<"*** User \'"<< sharedtable->clients[ID].username <<"\' entered from "<< sharedtable->clients[ID].IpPort <<". ***\n";
    SendMessage(message.str());

    stringstream ServerMsg;
    ServerMsg << "[Server] Client connected, id="<<ID<<", pid="<<getpid()<< endl;
    SendMessage(ServerMsg.str(), 0);

    sem_post(&sharedtable->ClientsLock);
}


void HandleDisconnection() {
    sem_wait(&sharedtable->ClientsLock);
    sharedtable->clients[ID].IsValid = false;
    sharedtable->ids_available[ID] = true;

    stringstream message;
    ClientInfo exitClient = sharedtable->clients[ID];
    message <<"*** User \'"<<exitClient.username <<"\' left. ***\n";
    SendMessage(message.str());

    stringstream ServerMsg;
    ServerMsg << "[Server] Client exited, id="<<ID<<", pid="<<exitClient.pid<< endl;
    SendMessage(ServerMsg.str(), 0);

    sem_post(&sharedtable->ClientsLock);
}

//----------------------------------------------------------------------------------------------------------------------
// SHELL FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void NpShell::run() {
    setenv("PATH", "bin:.", 1);
    
    while (true) {    
        cout << kBash << flush;

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

        if (DEBUG) {
            stringstream ServerMsg;
            ServerMsg << "[Server] #"<<ID<<" Run \"" << input <<"\", length="<<input.size()<<endl;
            SendMessage(ServerMsg.str(), 0);
        }
        if (input.empty()) continue; // Get user input, handle empty input
        if (input == "exit") {
            ClearUserPipe();
            break; 
        }

        string arg;
        stringstream ss(input);
        Command command;
        while (getline(ss, arg, ' ')) { // 以空格為分隔符讀取
            if (arg[0] == '|' || arg[0] == '!' || arg[0] == '>' || arg[0] == '<') {
                if ((stdin_stat = SetStdinFrom(command, input, arg)) == 1) { // if pipe is from user_pipe
                    if (!getline(ss, arg, ' ')) { arg.clear(); }// get next arg for output
                }
                stdout_stat = SetStdoutTo(command, input, arg, ss); // is user pipe, may need to read stdin again
                
                streampos position = ss.tellg(); // 紀錄存檔點用來倒回
                if (!getline(ss, arg, ' ')) { arg.clear(); }// get next arg for output
                if (arg[0] == '<') { stdin_stat = SetStdinFrom(command, input, arg); } // 確定要做user pipe再改command.Inpipe
                else { ss.seekg(position); } // 如果不是user pipe，返回存檔點，就當作沒讀過
                if (stdout_stat != 4) { UpdateNumberPipe(); } // "|" piping without updating NumberPipe
    
                OutputDEBUGMsg(command);
                ExecuteCommand(command);
                command.argv_vec.clear();
                continue;
            }
            command.argv_vec.push_back(arg);
            
            if (arg == "yell" || arg == "tell" || arg == "name" || arg == "block") { // 如指令為yell或tell，後面整句都是arg
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
    
            UpdateNumberPipe();
            OutputDEBUGMsg(command);
            ExecuteCommand(command);
            command.argv_vec.clear();  // 清空 vector
        }
    }

    return;
}

int NpShell::SetStdinFrom(Command &command, string input, string arg) {
    command.Inpipe = {STDIN_FILENO, null_fd};
    
    if (NumberPipe.count(0)) { // pipe fd_in
        command.Inpipe = NumberPipe[0];
        NumberPipe.erase(0); // if NumberPipe[0]有pipe，則需讀取pipe並刪除
        return 2; // numbered/ordinary pipe
    }

    if (arg[0] == '<') { // if is user pipe，則覆蓋掉其他pipe
        command.Inpipe[0] = command.Inpipe[1] = null_fd;
        command.sender_id = stoi(arg.substr(1));
        
        if (sharedtable->clients[command.sender_id].IsValid == false) { // sender user doesn't exist
            STDIN_MESSAGE << "*** Error: user #" << command.sender_id << " does not exist yet. ***\n";
            return -1;
        }
        string FIFO_name = USER_PIPE_DIR + to_string(command.sender_id) + "_to_" + to_string(ID);
        if (access(FIFO_name.c_str(), F_OK) < 0) { // user pipe doesn't exist
            STDIN_MESSAGE << "*** Error: the pipe #" << command.sender_id << "->#" << ID << " does not exist yet. ***\n";
            return -1;
        }

        int fifo_in = open(FIFO_name.c_str(), O_RDONLY | O_CLOEXEC);
        if (fifo_in < 0) { 
            perror("open"); 
            return -1;
        }
        
        STDIN_MESSAGE << "*** " << sharedtable->clients[ID].username << " (#" << ID << ") just received from ";
        STDIN_MESSAGE << sharedtable->clients[command.sender_id].username << " (#" << command.sender_id << ") by '" << input << "' ***\n";
        
        command.Inpipe[0] = fifo_in;
        command.fifo_pipe_name = FIFO_name;
        return 1; // user pipe
    }
    return 0;
}

int NpShell::SetStdoutTo(Command &command, string input, string arg, stringstream &ss) {
    //command.Outpipe = {null_fd, STDOUT_FILENO};
    command.fd_out = STDOUT_FILENO;
    command.fd_err = STDERR_FILENO;
    if (arg[0] == '>') { // '>'
        command.fd_out = null_fd; //command.Outpipe[1] = null_fd;
        if (arg.size() > 1) { // user pipe
            command.receiver_id = stoi(arg.substr(1));

            if (sharedtable->clients[command.receiver_id].IsValid == false) { // receiver user doesn't exist
                STDOUT_MESSAGE << "*** Error: user #" << command.receiver_id << " does not exist yet. ***\n";
                return -1;
            }
            string FIFO_name = USER_PIPE_DIR + to_string(ID) + "_to_" + to_string(command.receiver_id);
            if (access(FIFO_name.c_str(), F_OK) == 0) { //user pipe already exists
                STDOUT_MESSAGE << "*** Error: the pipe #" << ID << "->#" << command.receiver_id << " already exists. ***\n";
                return -1;
            }

            sem_wait(&sharedtable->UserPipeLock);
            if (sharedtable->userPipe[ID][command.receiver_id] == false) {
                if (mkfifo(FIFO_name.c_str(), 0666) == -1) {
                    STDOUT_MESSAGE << "*** Failed to create file: " << strerror(errno) << ". ***\n";
                    return -1; 
                }
                sharedtable->userPipe[ID][command.receiver_id] = true;
            }
            if ((command.fd_out = open(FIFO_name.c_str(), O_RDWR | O_CLOEXEC)) == -1) { 
                perror("open"); 
                return -1;
            }
            sem_post(&sharedtable->UserPipeLock);

            STDOUT_MESSAGE << "*** " << sharedtable->clients[ID].username << " (#" << ID << ") just piped \'" << input << "\' to "; 
            STDOUT_MESSAGE << sharedtable->clients[command.receiver_id].username << " (#" << arg.substr(1) << ") ***\n";

            return 1; // user pipe
        } else { // pipe to file
            string filename;
            getline(ss, filename, ' ');
            int file_fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
            if (file_fd < 0) {
                STDOUT_MESSAGE << "*** Failed to open file: " << strerror(errno) << ". ***\n";
                return -1; 
            }
            
            command.fd_out = file_fd;//command.Outpipe[1] = file_fd;
            return 2; // file pipe
        }
    } else if (arg[0] == '|' || arg[0] == '!') { // '|' or '!'
        int pipeNum = 0;
        if (arg.size() > 1) { pipeNum = stoi(arg.substr(1)); } //number pipe

        if (!NumberPipe.count(pipeNum)) { //create pipe
            array<int, 2> pipe_tmp;
            while (pipe(pipe_tmp.data()) == -1) {
                if (errno == EMFILE || errno == ENFILE) { wait(nullptr); } // 如沒有pipe則等wait，等其他process釋放資源
            }
            fcntl(pipe_tmp[0], F_SETFD, FD_CLOEXEC);
            fcntl(pipe_tmp[1], F_SETFD, FD_CLOEXEC);
            NumberPipe.emplace(pipeNum, pipe_tmp);
        }

        command.fd_out = NumberPipe[pipeNum][1];//command.Outpipe = NumberPipe[pipeNum][1];
        if (arg[0] == '!') { command.fd_err = NumberPipe[pipeNum][1]; }
        if (pipeNum) return 3; // number pipe
        return 4; // ordinary pipe
    }

    return 0;
}

void NpShell::OutputDEBUGMsg(Command command) {
    sem_wait(&sharedtable->ClientsLock);

    if (DEBUG) {
        stringstream DebugMsg;
        DebugMsg<< "[Server]\t(" << ((command.Inpipe[1]==null_fd)?"NULL":to_string(command.Inpipe[1])) << "->";
        DebugMsg<< ((command.Inpipe[0]==null_fd)?"NULL":to_string(command.Inpipe[0])) << ")->";
        DebugMsg<< command.argv_vec[0];
        DebugMsg<< "->" << ((command.fd_out==null_fd)?"NULL":to_string(command.fd_out)) << endl;
        DebugMsg<< "[Server]\tSTDIN_FLAG="<<stdin_stat<<" STDOUT_FLAG="<<stdout_stat<<endl;
        
        if (stdout_stat == 1) {
            DebugMsg<< "[Server]\tParent create: \""<<USER_PIPE_DIR<<ID<<"_to_"<<command.receiver_id<<"\"\n";
        }
        SendMessage(DebugMsg.str(), 0);
    }
    if (stdin_stat == -1) { cout << STDIN_MESSAGE.str() << flush; }
    else if (stdin_stat == 1) { SendMessage(STDIN_MESSAGE.str()); }
    if (stdout_stat == -1) { cout << STDOUT_MESSAGE.str() << flush; }
    else if (stdout_stat == 1) { SendMessage(STDOUT_MESSAGE.str()); }

    STDIN_MESSAGE.str("");       // 清除 buffer
    STDIN_MESSAGE.clear();       // 重置狀態標記（例如 eofbit）
    STDOUT_MESSAGE.str("");       // 清除 buffer
    STDOUT_MESSAGE.clear();       // 重置狀態標記（例如 eofbit）

    sem_post(&sharedtable->ClientsLock);
}

void NpShell::ExecuteCommand(Command command) {
    if (command.argv_vec.empty()) return; // 如果沒有指令，直接返回
    char** argv = command.Vec2Char();

    if (!strcmp(argv[0], "setenv") || !strcmp(argv[0], "printenv") || !strcmp(argv[0], "who") || !strcmp(argv[0], "yell") || !strcmp(argv[0], "tell") || !strcmp(argv[0], "name") || !strcmp(argv[0], "block")) {
        BuiltInCommand(command, argv);// Bulit-in commands
        SendMessage("[Server]\tBulitin command.\n", 0);
        return;
    } else {
        pid_t pid;
        while ((pid = fork()) == -1) { // 創建child process，如失敗(process滿了)則wait，等其他child釋放process資源
            if (errno == EAGAIN) { wait(nullptr); }
        }
        if (pid) { // parent process
            struct stat fd_stat;
            fstat(command.fd_out, &fd_stat); //fstat(command.Outpipe[1], &fd_stat);
            stringstream CloseMsg;
            CloseMsg << "[Server]\tParent close: ";
            
            if (command.Inpipe[0] != STDIN_FILENO && command.Inpipe[0] != null_fd) { 
                close(command.Inpipe[0]);
                CloseMsg << command.Inpipe[0] << ' ';
            } // pipe只在用完後回收
            if (command.Inpipe[1] != STDOUT_FILENO && command.Inpipe[1] != null_fd) { 
                close(command.Inpipe[1]);
                CloseMsg << command.Inpipe[1] << ' ';
            } // pipe只在用完後回收

            if (S_ISFIFO(fd_stat.st_mode) && fcntl(command.fd_out, F_GETFD) == 0) { // 1: to fifo, 2: to regular file
                close(command.fd_out);
                CloseMsg << command.fd_out << ' ';
            }
            if (stdout_stat == 1) { UserPipe[command.receiver_id] = command.fd_out; }

            if (!S_ISFIFO(fd_stat.st_mode)) { waitpid(pid, nullptr, 0); }
            CloseMsg << endl;
            SendMessage(CloseMsg.str(), 0);
        } else { //child process
            pid_t exec_pid = getpid();
            
            if (fork() == 0) { // 監督child執行進度
                stringstream TerminalMsg;
                waitpid(exec_pid, nullptr, 0); // 只等待目標進程
                if (stdin_stat == 1) { // user pipe stdin
                    close(command.Inpipe[0]); // 確保此時已經沒有指向fifo的port了

                    sem_wait(&sharedtable->ClientsLock);
                    union sigval value;
                    value.sival_int = ID;
                    sigqueue(sharedtable->clients[command.sender_id].pid, SIGRECV, value);
                    sem_post(&sharedtable->ClientsLock);
                }
                TerminalMsg << "[Server]\tChild run \""<<command.argv_vec[0]<<"\" terminated.\n";
                SendMessage(TerminalMsg.str(), 0);
                exit(0);
            }
            dup2(command.Inpipe[0], STDIN_FILENO);
            dup2(command.fd_out, STDOUT_FILENO);
            dup2(command.fd_err, STDERR_FILENO);
            if (execvp(argv[0], argv) < 0 && errno == ENOENT) { cerr << "Unknown command: [" << argv[0] << "]." << endl; }
            exit(0);
        }
    }

    return;
}

void NpShell::BuiltInCommand(Command command, char* argv[]) {
    sem_wait(&sharedtable->ClientsLock);

    // Environmant variable command
    if (!strcmp(argv[0], "setenv")) { setenv(argv[1], argv[2], 1);
    } else if (!strcmp(argv[0], "printenv")) { if (const char *env = getenv(argv[1])) { cout << env << endl; }
    // Client information command
    } else if (!strcmp(argv[0], "who")) {
        cout << "<ID>\t<nickname>\t<IP:port>\t<indicate me>"<<endl;
        for (int user_id=1; user_id<=MAX_USER_ID; user_id++) {
            if (sharedtable->clients[user_id].IsValid == true) {
                cout << user_id << '\t' << sharedtable->clients[user_id].username << '\t' << sharedtable->clients[user_id].IpPort;
                cout << ((user_id==ID) ? "\t<-me" : "") << endl;
            }
        }
    } else if (!strcmp(argv[0], "tell")) {
        int recv_id = stoi(argv[1]);
        if (recv_id > MAX_USER_ID || sharedtable->clients[recv_id].IsValid == false) {
            cout <<"*** Error: user #" <<recv_id <<" does not exist yet. ***"<<endl;
            sem_post(&sharedtable->ClientsLock);
            return;
        }
        stringstream message;
        message << "*** " << sharedtable->clients[ID].username << " told you ***: " << argv[2] << "\n";
        SendMessage(message.str(), recv_id, true);
    } else if (!strcmp(argv[0], "yell")) {
        stringstream message;
        message << "*** " << sharedtable->clients[ID].username << " yelled ***: " << argv[1] <<"\n";
        SendMessage(message.str(), -1, true);
    } else if (!strcmp(argv[0], "name")) {
        for (int user_id=1; user_id<=MAX_USER_ID; user_id++) {
            if (strcmp(argv[1], sharedtable->clients[user_id].username) == 0) {
                cout << "*** User \'" << argv[1] << "\' already exists. ***" << endl;
                sem_post(&sharedtable->ClientsLock);
                return;
            }
        }

        strcpy(sharedtable->clients[ID].username, argv[1]);
        sharedtable->clients[ID].username[strlen(argv[1])] = '\0';
        stringstream message;
        message << "*** User from " << sharedtable->clients[ID].IpPort << " is named \'" << argv[1] << "\'. ***\n";
        SendMessage(message.str());
    } else if (!strcmp(argv[0], "block")) {
        int block_id = stoi(argv[1]);
        sharedtable->Block[ID][block_id] = sharedtable->Block[block_id][ID] = true;
    }
    
    sem_post(&sharedtable->ClientsLock);
}

void NpShell::ClearUserPipe() {
    sem_wait(&sharedtable->UserPipeLock);
    
    string FIFO_name;
    stringstream ClearMsg;
    ClearMsg << "[Server] #" << ID << " unlinked \"";
    for (int sender_id=1; sender_id<=MAX_USER_ID; sender_id++) { // 刪除給自己的所有user pipe
        if (sharedtable->userPipe[sender_id][ID] == true) {
            union sigval value;
            value.sival_int = ID;
            sigqueue(sharedtable->clients[sender_id].pid, SIGRECV, value);
        }
    }
    for (int receiver_id=1; receiver_id<=MAX_USER_ID; receiver_id++) { // 刪除自己寄的所有user pipe
        if (sharedtable->userPipe[ID][receiver_id] == true) {
            FIFO_name = USER_PIPE_DIR + to_string(ID) + "_to_" + to_string(receiver_id);
            close(npshell.UserPipe[receiver_id]);
            unlink(FIFO_name.c_str());
            sharedtable->userPipe[ID][receiver_id] = false;
            
            ClearMsg << FIFO_name << "\" \"";
        }
    }
    sem_post(&sharedtable->UserPipeLock);
    ClearMsg << "\n";
    SendMessage(ClearMsg.str(), 0);
}

void NpShell::UpdateNumberPipe() { // NumberPipe的key值全部-1
    unordered_map<int, array<int, 2>> new_map;
    for (const auto[key, value] : NumberPipe) { new_map.emplace(key - 1, value); } // reduce pipeNum
    NumberPipe = move(new_map);
}

//----------------------------------------------------------------------------------------------------------------------
// SERVER FUNCTION
//----------------------------------------------------------------------------------------------------------------------

Server::Server(int qlen, int port) { 
    //qlen; (maximum length of the server request queue)
    bzero((char *)&sin, sizeof(sin)); 
    sin.sin_family = AF_INET; 
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port); 

    int enable = 1;
    // Allocate a socket, set socket option, bind the socket, listen to the socket; 
    if ((msock = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0 /*Stands for tcp*/)) < 0) { perror("socket"); }
    if (setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) { perror("setsockopt"); }
    if (bind(msock, (struct sockaddr *)&sin, sizeof(sin)) < 0) { close(msock); perror("bind"); }
    if (listen(msock, qlen) < 0) { close(msock); perror("listen"); }
    
    for (int i = 1; i <= MAX_USER_ID; i++) { sharedtable->ids_available[i] = true; } //放入所有尚能使用的id

    sharedtable->clients[0].IsValid = true;
    sharedtable->clients[0].pid = getpid();

    cout << "[Server] Server is running..." << endl;
}

void Server::accepting() {
    while (1) { 
        struct sockaddr_in fsin; // the address of a client
        socklen_t alen = sizeof(fsin); // length of client's address
        int ssock; // slave server socket
        if ((ssock = accept(msock, (struct sockaddr *) &fsin, &alen)) < 0) { 
            if (errno == EINTR) continue;
        }

        int id_tmp;
        for (id_tmp=1; id_tmp<=MAX_USER_ID; id_tmp++) {
            if (sharedtable->ids_available[id_tmp] == true) break;
        }

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGMSG);
        sigprocmask(SIG_BLOCK, &mask, nullptr);     // 先 block 掉

        pid_t pid;
        while ((pid = fork()) == -1) { // 創建child process，如失敗(process滿了)則wait，等其他child釋放process資源
            if (errno == EAGAIN) { wait(nullptr); }
        }
        
        if (pid > 0) { // parent process
            sigprocmask(SIG_UNBLOCK, &mask, nullptr);
            (void) close(ssock); 
        } else { //child process
            (void) close(msock);
            ID = id_tmp;
            dup2(ssock, STDIN_FILENO);
            dup2(ssock, STDOUT_FILENO);
            dup2(ssock, STDERR_FILENO);
            HandleConnection(ssock);
            sigprocmask(SIG_UNBLOCK, &mask, nullptr);

            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sa.sa_sigaction = SIGRECVHandler;
            sigaction(SIGRECV, &sa, NULL);

            signal(SIGCHLD, SIG_IGN);

            npshell.run();
            HandleDisconnection();
            exit(0);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// MAIN FUNCTION 
//----------------------------------------------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    signal(SIGCHLD, reaper);

    //signal(SIGINT, ServerTeardown);   // Ctrl+C
    //signal(SIGTERM, ServerTeardown);  // kill pid
    //signal(SIGQUIT, ServerTeardown);  // Ctrl+'\'

    signal(SIGMSG, SIGMSGHandler); // 註冊 handler

    setenv("PATH", "bin:.", 1);
    null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

    int port;
    if (argc == 2) port = stol(argv[1]);
    else {
        cout << "Port!!!"<<endl;
        return 1;
    }

    if ((addr = mmap(NULL, sizeof(SharedTable), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0)) == MAP_FAILED) { perror("mmap"); }
    memset(addr, 0, sizeof(SharedTable)); //清空初始化
    sharedtable = reinterpret_cast<SharedTable*>(addr);
    for (int user_id=0; user_id<=MAX_USER_ID; user_id++) { sem_init(&sharedtable->msgBoxes[user_id].lock, 1, 1); }
    sem_init(&sharedtable->ClientsLock, 1, 1);
    sem_init(&sharedtable->UserPipeLock, 1, 1);

    server = new Server(5, port);
    server->accepting();
    
    return 0;
}