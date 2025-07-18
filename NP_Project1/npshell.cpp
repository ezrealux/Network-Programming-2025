#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unordered_map>
#include <array>

using namespace std;

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

void SetStdinFrom(Command &command, unordered_map<int, array<int, 2>> &pipeMap) {
    //cout << "pipeMap[0]~"<<pipeMap.count(0)<<"~("<<pipeMap[0][1]<<"->"<<pipeMap[0][0]<<") "<<endl;
    if (pipeMap.count(0)) { // pipe fd_in
        command.pipe = pipeMap[0];
        pipeMap.erase(0);
    } else { command.pipe = {STDIN_FILENO, STDOUT_FILENO}; }
    //cout << "command.pipe("<<command.pipe[1]<<"->"<<command.pipe[0]<<") "<<endl;
}

void SetStdoutTo(Command &command, unordered_map<int, array<int, 2>> &pipeMap, string arg, stringstream &ss) {
    if (arg.empty()) {
        command.fd_out = STDOUT_FILENO;
        command.fd_err = STDERR_FILENO;
        return;
    }

    array<int, 2> pipe_tmp;
    int pipeNum = 0;
    if (arg.size() > 1) { pipeNum = stoi(arg.substr(1)); }
    cout <<pipeNum<<"~~~";
    
    if (arg[0] == '>') { // '>'
        string filename;
        getline(ss, filename, ' ');
        if ((command.fd_out = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0777)) == -1) {
            cerr << "failed to open file: " << strerror(errno) << endl;
            exit(0);
        }
        cout<<command.pipe[0]<< " -> \""<<command.argv_vec[0]<<"\" -> "<<command.fd_out<<" -> \""<<filename<< "\"\n";
    } else { // '|' or '!'
        cout<<!pipeMap.count(pipeNum)<<"->";
        if (!pipeMap.count(pipeNum)) { //create pipe
            while (pipe(pipe_tmp.data()) == -1) {
                if (errno == EMFILE || errno == ENFILE) { wait(nullptr); } // 如沒有pipe則等wait，等其他process釋放資源
            }
            fcntl(pipe_tmp[0], F_SETFD, FD_CLOEXEC);
            fcntl(pipe_tmp[1], F_SETFD, FD_CLOEXEC);
            pipeMap.emplace(pipeNum, pipe_tmp);
        }
        cout <<pipe_tmp[0]<<" "<<pipe_tmp[1]<<endl;

        if (arg[0] == '|') {
            command.fd_out = pipeMap[pipeNum][1];
            cout<<command.pipe[0]<< " -> \""<<command.argv_vec[0]<<"\" -> "<<command.fd_out<<" -> "<<pipeMap[pipeNum][0]<<"\n";
        } else {
            command.fd_out = pipeMap[pipeNum][1];
            command.fd_err = pipeMap[pipeNum][1];
        }
    }
}

void ExecuteCommand(Command command) {
    if (command.argv_vec.empty()) return; // 如果沒有指令，直接返回
    char** argv = command.Vec2Char();
    
    // Bulit-in commands
    if (!strcmp(argv[0], "exit")) { exit(0); }
    if (!strcmp(argv[0], "setenv")) {
        setenv(argv[1], argv[2], 1);
        return;
    }
    if (!strcmp(argv[0], "printenv")) {
        if (const char *env = getenv(argv[1])) {
            cout << env << endl;
        }
        return;
    }

    pid_t pid = fork();  // 創建child process
    if (pid < 0) { wait(nullptr); } // 創建失敗(process滿了)則wait，等其他child釋放process資源
    else {
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
        }
    }
}

void UpdatePipemap(unordered_map<int, array<int, 2>> &pipeMap) { // PipeMap的key值全部-1
    unordered_map<int, array<int, 2>> new_map;
    for (const auto[key, value] : pipeMap) { new_map.emplace(key - 1, value); } // reduce pipeNum
  
    pipeMap = move(new_map);
}

void CommandHandling(Command &command, unordered_map<int, array<int, 2>> &pipeMap, string &arg, stringstream &ss) {
    SetStdinFrom(command, pipeMap);
    SetStdoutTo(command, pipeMap, arg, ss);
    if (!command.argv_vec.empty()) ExecuteCommand(command);
    command.argv_vec.clear();  // 清空 vector
    if (arg != "|") UpdatePipemap(pipeMap);
}

int main() {
    signal(SIGCHLD, SIG_IGN);
    setenv("PATH", "bin:.", 1);
    string input;
    unordered_map<int, array<int, 2>> pipeMap;
    
    while (true) {
        //輸入一行prompt
        cout << "% " << flush;
        if (!getline(cin, input) || input.empty()) continue; // Get user input, handle empty input
        
        string arg;
        stringstream ss(input);
        Command command;
        while (getline(ss, arg, ' ')) { // 以空格為分隔符讀取
            if (arg[0] == '|' || arg[0] == '!' || arg[0] == '>') { // 遇到|, !, >，輸出 args
                CommandHandling(command, pipeMap, arg, ss);
                continue;
            }
            command.argv_vec.push_back(arg);
        }
        arg.clear();

        if (!command.argv_vec.empty()) { CommandHandling(command, pipeMap, arg, ss); }
    }
    return 0;
}