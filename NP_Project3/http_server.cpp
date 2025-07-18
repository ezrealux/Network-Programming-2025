#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <boost/asio.hpp>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <map>

using namespace std;
using boost::asio::ip::tcp;

//----------------------------------------------------------------------------------------------------------------------
// OBJECTIVE
//----------------------------------------------------------------------------------------------------------------------

class Session : public std::enable_shared_from_this<Session>{
    // https://kheresy.wordpress.com/2018/08/08/enable_shared_from_this/
	public:
		Session(tcp::socket socket) : socket_(std::move(socket)){}
		void start(){ do_read(); }

	private:	
		tcp::socket socket_;
		enum { max_length = 2048 };
		char data_[max_length];
		map<string, string> env;
		string path;
		int state;

		void do_read();
		void do_exec(int length);
		
		void fillEnv(string str);
		void dup2Client(int fd);
		char** stoc(vector<string> argv_vec);
};

class Server{
	public:
        // 建立server的同時初始化acceptor_ 
        // 因acceptor沒有預設constructor(不能建一個空的acceptor_)，server建立時一定要立刻設定好
		Server(boost::asio::io_context& io_context, short port) : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)){ do_accept(); }
        
	private:
		tcp::acceptor acceptor_;

		void do_accept(){
            //do_accept: 建立一個async_accept等待接受->結束
            //async_accept: 建立一個Session->再跑一次do_accept
			acceptor_.async_accept(
                // lambda 表達式(似python)
                // [ capture_list ] ( parameter_list ) -> return_type { function_body }
				[this](boost::system::error_code ec, tcp::socket socket) {
                    // https://kheresy.wordpress.com/2012/03/03/c11_smartpointer_p1/
                    // https://kheresy.wordpress.com/2012/03/05/c11_smartpointer_p2/
                    // move(): 轉移物件的所有權到另一個pointer
					if (!ec){ std::make_shared<Session>(std::move(socket))->start(); }
					else { cerr << "Accept Error: " << ec.message() << " (" << ec.value() << ")" << endl; }
					do_accept();
				}
			);
		}
};

//----------------------------------------------------------------------------------------------------------------------
// Session FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void Session::do_read(){
	// 等於 auto self = shared_from_this();
	auto self(shared_from_this());
	socket_.async_read_some(
		boost::asio::buffer(data_, max_length), 
		[this, self](boost::system::error_code ec, std::size_t length){
			if (!ec){
				pid_t pid;
				while (true) {
					while (waitpid(-1, &state, WNOHANG) > 0); // 回收殭屍子程序
					pid = fork();
					if (pid < 0) {
						usleep(1000); // fork 失敗，sleep 一下再重試
						continue;
					}
					break; // fork 成功（不管是父或子都break）
				}

				if (pid == 0) { do_exec(length); } // 子程序做事情
				else {
					socket_.close();
					while(waitpid(-1, &state, WNOHANG) > 0);
				} 
			} else { cerr << "Read Error: " << ec.message() << " (" << ec.value() << ")" << endl; }
		}
	);
}

void Session::do_exec(int length) {
	data_[length] = '\0';
	
	//env
	fillEnv(string(data_));
	memset(data_, '\0', sizeof(data_)); // 全填上\0 = 陣列清空
	for (auto e : env) { setenv(e.first.c_str(), e.second.c_str(), 1); }
	//for (auto a : env) cout << a.first << ": " << a.second << endl;
	
	//dup2
	int fd = socket_.native_handle();
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);

	//close: 
	socket_.close();

	//exec
	cout << "HTTP/1.1 200 OK\r\n" << flush;
	cout << "Server: http_server\r\n" << flush;
	path = "." + path;
	char **argv = stoc({path});
	int e = execv(path.c_str(), argv);
	if(e == -1){ exit(1); }
	exit(0);
}

void Session::fillEnv(string str){
    // 取出第一行
	istringstream request_stream(str);
    string line;
    getline(request_stream, line);
    
    // HTTP Request Line: METHOD URI PROTOCOL
    istringstream line_stream(line);
    line_stream >> env["REQUEST_METHOD"] >> env["REQUEST_URI"] >> env["SERVER_PROTOCOL"];

    // 將REQUEST_URI拆成path跟QUERY_STRING
    size_t pos = env["REQUEST_URI"].find('?');
    if (pos != string::npos) { // if找的到'?'
        env["QUERY_STRING"] = env["REQUEST_URI"].substr(pos + 1); // 後面的 query
		path = env["REQUEST_URI"].substr(0, pos); // 前面的純 URI
    } else {
        env["QUERY_STRING"] = "";
		path = env["REQUEST_URI"];
    }

	// HTTP_HOST
	while (getline(request_stream, line) && line != "\r") {
        if (line.back() == '\r') line.pop_back(); // 去掉 '\r'
        size_t colon = line.find(':');
        if (colon != string::npos) {
            string header_name = line.substr(0, colon);
            string header_value = line.substr(colon + 1);
            while (!header_value.empty() && (header_value[0] == ' ')) { header_value.erase(0, 1); } // 去掉空白
            if (header_name == "Host") {
                env["HTTP_HOST"] = header_value;
				break;
            }
        }
    }

	env["SERVER_ADDR"] = socket_.local_endpoint().address().to_string();
	env["SERVER_PORT"] = to_string(socket_.local_endpoint().port());
	env["REMOTE_ADDR"] = socket_.remote_endpoint().address().to_string();
	env["REMOTE_PORT"] = to_string(socket_.remote_endpoint().port());
}

char** Session::stoc(vector<string> argv_vec){ //vector<string> to char[][]
	// vector<string>& argv_vec => char* argv_vec[] (C-style 陣列)
	char** argv = new char*[argv_vec.size() + 1];
	for (size_t i = 0; i < argv_vec.size(); i++) {
		argv[i] = strdup(argv_vec[i].c_str());
	}
	argv[argv_vec.size()] = nullptr; // execvp 需要 nullptr 結尾

	return argv;
}

//----------------------------------------------------------------------------------------------------------------------
// MAIN FUNCTION
//----------------------------------------------------------------------------------------------------------------------

int main(int argc, char* argv[]){
	try{
		if (argc != 2){
			std::cerr << "Usage: async_tcp_echo_server <port>\n";
			return 1;
		}
		// 開一個控制中心，等一下讓所有 I/O 都掛進來處理
		boost::asio::io_context io_context;
		// 把io_context傳進server，讓io_context能監聽server的事件
		Server s(io_context, std::atoi(argv[1]));
		// 開始跑 持續監聽所有async事件（如 accept、read、write）直到所有的async任務結束或stop()被叫了
		io_context.run();
	} catch (std::exception& e){
		std::cerr << "Exception: " << e.what() << "\n";
	}
	return 0;
}