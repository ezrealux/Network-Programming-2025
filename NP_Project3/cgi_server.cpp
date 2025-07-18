#include <stdlib.h>  // for getenv() setenv()
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>
#include <utility>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <thread>

using boost::asio::ip::tcp;
using namespace std;

//----------------------------------------------------------------------------------------------------------------------
// OBJECTIVE
//----------------------------------------------------------------------------------------------------------------------

struct shellInfo{
	string host;
	string port;
	string cmdFile;
};

class ShellClient : public std::enable_shared_from_this<ShellClient>{
	public:
    ShellClient(map<unsigned int, shellInfo> shellServer, std::shared_ptr<tcp::socket> socket, boost::asio::io_context& io_context, unsigned int index)
         : shellServer(shellServer), OutSocket_(socket), resolver(io_context), InSocket_(io_context), index(index){}
		void start(){ do_resolve(); }

	private:
        map<unsigned int, shellInfo> shellServer;
        std::shared_ptr<tcp::socket> OutSocket_; // single process中，不可用dup更換cout，需保留out socket的位置
		tcp::resolver resolver;
		tcp::socket InSocket_;
		int index;
        
		tcp::resolver::results_type endpoint_;
		ifstream in;
		enum { max_length = 40960 };
		char data_[max_length];

		void do_resolve();
		void do_connect();
		void do_read();
		void do_write();
		void output_message(string content);
		void output_command(string content);
        void escape(string &src);
};

class Session : public std::enable_shared_from_this<Session>{
    // https://kheresy.wordpress.com/2018/08/08/enable_shared_from_this/
	public:
		Session(tcp::socket socket) : socket_(std::make_shared<tcp::socket>(std::move(socket))){}
		void start(){ do_read(); }

	private:	
		std::shared_ptr<tcp::socket> socket_;
		enum { max_length = 2048 };
		char data_[max_length];
		map<string, string> env;
		string path;
		int state;

		void do_read();
        void do_write(string out);
		void fillEnv(string str);
		char** stoc(vector<string> argv_vec);
};

class Server{
	public:
        // 建立Server的同時初始化acceptor_ 
        // 因acceptor沒有預設constructor(不能建一個空的acceptor_)，Server建立時一定要立刻設定好
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
					//cout << "Accepted..." << endl;
					do_accept();
				}
			);
		}
};

string panel();
string http(map<unsigned int, shellInfo> &shellServer);
void getShellServerInfo(map<unsigned int, shellInfo> &shellServer, map<string, string> &env);
void console(std::shared_ptr<tcp::socket> sessionSocket_, map<string, string> &env);

//----------------------------------------------------------------------------------------------------------------------
// SHELL FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void ShellClient::do_resolve(){
	auto self(shared_from_this());
	resolver.async_resolve(
	// async_resolve(host, port, bind): 解析連線主機與port，得出endpoint
		shellServer[index].host, shellServer[index].port,
		[this, self](boost::system::error_code ec, tcp::resolver::results_type result){
			if(!ec){
				memset(data_, '\0', sizeof(data_)); // 清空data_
				
				endpoint_ = result;
				do_connect();
			}else{
				cerr << "Resolve Error: " << ec.message() << " (" << ec.value() << ")" << endl;
				InSocket_.close(); // 如果無法解析，就趕快關了這個socket
			}
		}
	);
}

void ShellClient::do_connect(){
	auto self(shared_from_this());
	boost::asio::async_connect(
	// 連接endpoint到insocket到insocket
		InSocket_, endpoint_,
		[this, self](boost::system::error_code ec, tcp::endpoint ed){
			if(!ec){
				memset(data_, '\0', sizeof(data_)); // 清空data_
				in.open("./test_case/" + shellServer[index].cmdFile);
				if(!in.is_open()){
					cout << shellServer[index].cmdFile << " open fail\n";
					InSocket_.close();
				}
				
				do_read();
			} else {
				cerr << "Connect Error: " << ec.message() << " (" << ec.value() << ")" << endl;
				InSocket_.close();
			}
		}
	);
}

void ShellClient::do_read(){
	auto self(shared_from_this());
	InSocket_.async_read_some(
		boost::asio::buffer(data_, max_length),  // 填入data_
		[this, self](boost::system::error_code ec, std::size_t length){
			if (!ec){
				if(length == 0) return;
				data_[length] = '\0';
				string msg = string(data_); // 複製、清空、再輸出，避免輸出途中有人更改data_
				memset(data_, '\0', sizeof(data_)); // 清空data_
				
				output_message(msg);
				
				if(msg.find("% ") != string::npos){ do_write(); } // 如果npshell輸出了kbash = 又可以輸入指令了
				else{ do_read(); }
			} else {
				//cerr << "Shell read Error: " << ec.message() << " (" << ec.value() << ")" << endl;
				InSocket_.close();
			}
		}
	);
}

void ShellClient::do_write(){
	auto self(shared_from_this());
	string cmd;
	getline(in, cmd); // 從test case裡再抓一行出來 塞到cmd內
	cmd.push_back('\n');
	output_command(cmd); // 把command顯示在螢幕上
	boost::asio::async_write( // 把結果寫到shell內
		InSocket_, boost::asio::buffer(cmd, cmd.size()),
		[this, self](boost::system::error_code ec, std::size_t length){
			if (!ec){ do_read(); } // 向npshell打完指令後繼續讀
			else { cerr << "Write Error: " << ec.message() << " (" << ec.value() << ")" << endl; }
		}
	);
}

void ShellClient::output_message(string content){
    escape(content);
    string out = "<script>document.getElementById('s" + to_string(index) + "').innerHTML += '" + content + "';</script>\n";
    boost::asio::write(*OutSocket_, boost::asio::buffer(out));
}

void ShellClient::output_command(string content){
    escape(content);
    string out = "<script>document.getElementById('s" + to_string(index) + "').innerHTML += '<b>" + content + "</b>';</script>\n";
    boost::asio::write(*OutSocket_, boost::asio::buffer(out));
}

void ShellClient::escape(string &src) { // 特殊符號轉成html安全字串
	boost::replace_all(src, "&", "&amp;");
	boost::replace_all(src, "\r", "");
	boost::replace_all(src, "\n", "&NewLine;");
	boost::replace_all(src, "\'", "&apos;");
	boost::replace_all(src, "\"", "&quot;");
	boost::replace_all(src, "<", "&lt;");
	boost::replace_all(src, ">", "&gt;");
}

//----------------------------------------------------------------------------------------------------------------------
// SESSION FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void Session::do_read() { // Read Request
	auto self(shared_from_this());
	socket_->async_read_some(
		boost::asio::buffer(data_, max_length), 
		[this, self](boost::system::error_code ec, std::size_t length){
			if (!ec){
                data_[length] = '\0'; // 假設可一次讀完整個request
                fillEnv(string(data_));
                memset(data_, '\0', sizeof(data_)); // 全填上\0 = 陣列清空

                string header = "HTTP/1.1 200 OK\r\nServer: http_server\r\n\r\n";
                if (path == "/panel.cgi") {
                    do_write(header);
                    do_write(panel());
                    //socket_.close();
                } else if (path == "/console.cgi") { // console不用close()，因為還要繼續r/w
                    do_write(header);
                    std::thread t(console, socket_, std::ref(env));
                    t.detach();
                } else{
                    header = "HTTP/1.1 404 NOT FOUND\r\n\r\n";
                    do_write(header);
                    //socket_.close();
                }

                //cout << "do_read()";
                do_read();
			}// else { cerr << "Session read Error: " << ec.message() << " (" << ec.value() << ")" << endl; }
		}
	);
}

void Session::do_write(string out) {
    auto self(shared_from_this());
    boost::asio::async_write(
        *socket_, boost::asio::buffer(out, out.size()),
        [this, self](boost::system::error_code ec, std::size_t length){
            if (!ec){ }
        }
    );
}

void Session::fillEnv(string str) {
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

	env["SERVER_ADDR"] = socket_->local_endpoint().address().to_string();
	env["SERVER_PORT"] = to_string(socket_->local_endpoint().port());
	env["REMOTE_ADDR"] = socket_->remote_endpoint().address().to_string();
	env["REMOTE_PORT"] = to_string(socket_->remote_endpoint().port());
}

char** Session::stoc(vector<string> argv_vec) {
	// vector<string>& argv_vec => char* argv_vec[] (C-style 陣列)
	char** argv = new char*[argv_vec.size() + 1];
	for (size_t i = 0; i < argv_vec.size(); i++) {
		argv[i] = strdup(argv_vec[i].c_str());
	}
	argv[argv_vec.size()] = nullptr; // execvp 需要 nullptr 結尾

	return argv;
}

//----------------------------------------------------------------------------------------------------------------------
// HTML FUNCTION
//----------------------------------------------------------------------------------------------------------------------

string panel() {
    ostringstream oss;
    
    ostringstream test_case_menu;
    ostringstream host_menu;
    for (int i = 1; i <= 5; i++) {
        string test_case = "t" + to_string(i) + ".txt";
        test_case_menu << "<option value=\"" << test_case << "\">" << test_case << "</option>";
    }
    for (int i = 1; i <= 12; i++) {
        string host = "nplinux" + to_string(i);
        host_menu << "<option value=\"" << host << ".cs.nycu.edu.tw\">" << host << "</option>";
    }

    oss << R"(
    <!DOCTYPE html>
        <html lang="en">
        <head>
            <title>NP Project 3 Panel</title>
            <link
                rel="stylesheet"
                href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css"
                integrity="sha384-TX8t27EcRE3e/ihU7zmQxVncDAy5uIKz4rEkgIXeMed4M0jlfIDPvg6uqKI2xXr2"
                crossorigin="anonymous"
            />
            <link
                href="https://fonts.googleapis.com/css?family=Source+Code+Pro"
                rel="stylesheet"
            />
            <link
                rel="icon"
                type="image/png"
                href="https://cdn4.iconfinder.com/data/icons/iconsimple-setting-time/512/dashboard-512.png"
            />
            <style>
            * {
                font-family: 'Source Code Pro', monospace;
            }
            </style>
        </head>
        <body class="bg-secondary pt-5">
            <form action="console.cgi" method="GET">
                <table class="table mx-auto bg-light" style="width: inherit">
                    <thead class="thead-dark">
                        <tr>
                            <th scope="col">#</th>
                            <th scope="col">Host</th>
                            <th scope="col">Port</th>
                            <th scope="col">Input File</th>
                        </tr>
                    </thead>
                    <tbody>
    )";
    for (int i = 0; i < 5; ++i) {
        oss << R"(      <tr>
                            <th scope="row" class="align-middle">Session)" << (i+1) << R"(</th>
                            <td>
                            <div class="input-group">
                                <select name="h)" << i << R"(" class="custom-select">
                                <option></option>)" << host_menu.str() << R"(
                                </select>
                                <div class="input-group-append">
                                    <span class="input-group-text">.cs.nycu.edu.tw</span>
                                </div>
                            </div>
                            </td>
                            <td>
                                <input name="p)" <<i<< R"(" type="text" class="form-control" size="5" />
                            </td>
                            <td>
                                <select name="f)" <<i<< R"(" class="custom-select">
                                    <option></option>)" << test_case_menu.str() << R"(
                                </select>
                            </td>
                        </tr>)";
    }
    oss << R"(          <tr>
                            <td colspan="3"></td>
                            <td>
                                <button type="submit" class="btn btn-info btn-block">Run</button>
                            </td>
                        </tr>
                    </tbody>
                </table>
            </form>
        </body>
    </html>
    )";

    return oss.str();
}

string http(map<unsigned int, shellInfo> &shellServer) {
    ostringstream oss;

    oss << R"(<!DOCTYPE html>
    <html lang="en">
        <head>
            <meta charset="UTF-8" />
            <title>NP Project 3 Sample Console</title>
            <link rel="stylesheet"
                href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css"
                integrity="sha384-TX8t27EcRE3e/ihU7zmQxVncDAy5uIKz4rEkgIXeMed4M0jlfIDPvg6uqKI2xXr2"
                crossorigin="anonymous" />
            <link href="https://fonts.googleapis.com/css?family=Source+Code+Pro" rel="stylesheet" />
            <link rel="icon" type="image/png" href="https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png" />
            <style>
                * {
                font-family: 'Source Code Pro', monospace;
                font-size: 1rem !important;
                }
                body {
                background-color: #212529;
                }
                pre {
                color: #cccccc;
                }
                b {
                color: #01b468;
                }
            </style>
        </head>
        <body>
            <table class="table table-dark table-bordered">
                <thead>
                    <tr>
    )";
    for (size_t i = 0; i < shellServer.size(); ++i) {
        oss << "        <th scope=\"col\">" << shellServer[i].host << ":" << shellServer[i].port << "</th>\n";
    }
    oss << R"(      </tr>
                </thead>
                <tbody>
                    <tr>
    )";
    for (size_t i = 0; i < shellServer.size(); ++i) {
        oss << "        <td><pre id=\"s" << i << "\" class=\"mb-0\"></pre></td>\n";
    }
    oss << R"(      </tr>
                </tbody>
            </table>
        </body>
    </html>
    )";

    return oss.str();
}
    
//----------------------------------------------------------------------------------------------------------------------
// OTHERS FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void getShellServerInfo(map<unsigned int, shellInfo> &shellServer, map<string, string> &env) {
    istringstream ss(env["QUERY_STRING"]);
    string token;
    while (getline(ss, token, '&')) {  // 每次讀一個 key=value
        if (token.empty()) continue;
        size_t pos = token.find('=');
        if (pos == string::npos) continue;
        string key = token.substr(0, pos);
        string value = token.substr(pos + 1);
        cerr << key << " " << value << endl;
        char type = key[0];     	// 'h', 'p', 'f'
        int index = key[1] - '0'; 	// '0' ~ '4'
		if (value.empty()) break;

		switch (type) {
			case 'h':{
				shellServer[index].host = value;
				break;
			}
			case 'p':{
				shellServer[index].port = value;
				break;
			}
			case 'f':{
				shellServer[index].cmdFile = value;
				break;
			}
		}
    }
}

void console(std::shared_ptr<tcp::socket> sessionSocket_, map<string, string> &env) {
    try{
        cerr <<"console"<<endl;
        map<unsigned int, shellInfo> shellServer;
		getShellServerInfo(shellServer, env);
        boost::asio::write(*sessionSocket_, boost::asio::buffer(http(shellServer)));
		boost::asio::io_context io_context;
		for(unsigned int i = 0; i < shellServer.size(); i++){ // 多個npshell指向同個
			std::make_shared<ShellClient>(shellServer, sessionSocket_ ,io_context, i)->start();
		}
		io_context.run();
		//sessionSocket_.close(); // 沒用move把socket的控制權放給OutSocket_，shellClient結束後io_context結束，仍可從這裡close
	} catch (std::exception& e){
		cerr << "Exception: " << e.what() << "\n";
	}
}

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
		//cout << "Start running..." << endl;
		io_context.run();
	} catch (std::exception& e){
		std::cerr << "Exception: " << e.what() << "\n";
	}
	return 0;
}