#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <utility>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <map>

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

map<string, string> env;
map<unsigned int, shellInfo> shellServer;
vector<string> envSet = {"REQUEST_METHOD", "REQUEST_URI", "QUERY_STRING", "SERVER_PROTOCOL", "HTTP_HOST", "SERVER_ADDR", "SERVER_PORT", "REMOTE_ADDR", "REMOTE_PORT"};

class HelloTimer  {
public:
    HelloTimer(boost::asio::io_context& io_context, int size) : timer_(io_context, boost::posix_time::seconds(2)), MAX(size){} // 初始2秒
	void start(){ hello(); }

private:
    boost::asio::deadline_timer timer_;
	int MAX;
    
	void hello() {
        timer_.expires_from_now( boost::posix_time::seconds(2));
        timer_.async_wait(
			[this](boost::system::error_code ec){
				if(!ec){
					for (int index=0; index<MAX; index++) {
						cout << "<script>document.getElementById(\'s" << index << "\').innerHTML += \'<b>Hello</b>\';</script>\n" << flush;
					}
					hello(); 
				}
			}
		);
    }
};

class ShellClient : public std::enable_shared_from_this<ShellClient>{
	public:
		ShellClient(boost::asio::io_context& io_context, int index) : resolver(io_context), socket_(io_context), index(index){}
		void start(){ do_resolve(); }

	private:
		tcp::resolver resolver;
		tcp::socket socket_;
		unsigned int index;
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
				socket_.close();
			}
		}
	);
}

void ShellClient::do_connect(){
	auto self(shared_from_this());
	boost::asio::async_connect(
	// 連接endpoint	
		socket_, endpoint_,
		[this, self](boost::system::error_code ec, tcp::endpoint ed){
			if(!ec){
				memset(data_, '\0', sizeof(data_)); // 清空data_
				in.open("./test_case/" + shellServer[index].cmdFile);
				if(!in.is_open()){
					cout << shellServer[index].cmdFile << " open fail\n";
					socket_.close();
				}
				
				do_read();
			} else {
				cerr << "Connect Error: " << ec.message() << " (" << ec.value() << ")" << endl;
				socket_.close();
			}
		}
	);
}

void ShellClient::do_read(){
	auto self(shared_from_this());
	socket_.async_read_some(
		boost::asio::buffer(data_, max_length),  // 填入data_
		[this, self](boost::system::error_code ec, std::size_t length){
			if (!ec){
				if(length == 0) return;
				data_[length] = '\0';
				string msg = string(data_);
				memset(data_, '\0', sizeof(data_)); // 清空data_
				
				output_message(msg);
				
				if(msg.find("% ") != string::npos){ do_write(); } // 如果npshell輸出了kbash = 又可以輸入指令了
				else{ do_read(); }

			} else {
				cerr << "Read Error: " << ec.message() << " (" << ec.value() << ")" << endl;
				socket_.close();
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
	boost::asio::async_write(
		socket_, boost::asio::buffer(cmd, cmd.size()),
		[this, self](boost::system::error_code ec, std::size_t length){
			if (!ec){ do_read(); } // 向npshell打完指令後繼續讀
			else { cerr << "Write Error: " << ec.message() << " (" << ec.value() << ")" << endl; }
		}
	);
}

void ShellClient::output_message(string content){
	escape(content);
	cout << "<script>document.getElementById(\'s" << index << "\').innerHTML += \'" << content << "\';</script>\n" << flush;
}

void ShellClient::output_command(string content){
	escape(content);
	cout << "<script>document.getElementById(\'s" << index << "\').innerHTML += \'<b>" << content << "</b>\';</script>\n" << flush;
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
// UTILITY FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void getShellServerInfo(){
    istringstream ss(env["QUERY_STRING"]);
    string token;
    while (getline(ss, token, '&')) {  // 每次讀一個 key=value
        if (token.empty()) continue;
        size_t pos = token.find('=');
        if (pos == string::npos) continue;
        string key = token.substr(0, pos);
        string value = token.substr(pos + 1);

        char type = key[0];     	// 'h', 'p', 'f'
        int index = key[1] - '0'; 	// '0' ~ '4'
		if (value.empty()) break;

		switch (type)
		{
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

void http(){
cout << "<!DOCTYPE html>" 																							<< '\n';
cout << "<html lang=\"en\">" 																						<< '\n';
cout << "	<head>" 																								<< '\n';
cout << "		<meta charset=\"UTF-8\" />" 																		<< '\n';
cout << "		<title>NP Project 3 Sample Console</title>" 														<< '\n';
cout << "		<link" 																								<< '\n';
cout << "			rel=\"stylesheet\""																				<< '\n';
cout << "			href=\"https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css\"" 				<< '\n';
cout << "			integrity=\"sha384-TX8t27EcRE3e/ihU7zmQxVncDAy5uIKz4rEkgIXeMed4M0jlfIDPvg6uqKI2xXr2\"" 			<< '\n';
cout << "			crossorigin=\"anonymous\"" 																		<< '\n';
cout << "		/>"																									<< '\n';
cout << "		<link" 																								<< '\n';
cout << "			href=\"https://fonts.googleapis.com/css?family=Source+Code+Pro\"" 								<< '\n';
cout << "			rel=\"stylesheet\"" 																			<< '\n';
cout << "		/>" 																								<< '\n';
cout << "		<link" 																								<< '\n';
cout << "			rel=\"icon\"" 																					<< '\n';
cout << "			type=\"image/png\"" 																			<< '\n';
cout << "			href=\"https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png\"" 		<< '\n';
cout << "		/>" 																								<< '\n';
cout << "		<style>" 																							<< '\n';
cout << "			* {" 																							<< '\n';
cout << "				font-family: \'Source Code Pro\', monospace;" 												<< '\n';
cout << "				font-size: 1rem !important;" 																<< '\n';
cout << "			}" 																								<< '\n';
cout << "      		body {" 																						<< '\n';
cout << "        		background-color: #212529;" 															   << '\n';
cout << "      		}" 																								<< '\n';
cout << "      		pre {" 																							<< '\n';
cout << "        		color: #cccccc;" 																		   << '\n';
cout << "      		}" 																								<< '\n';
cout << "      		b {" 																							<< '\n';
cout << "        		color: #01b468;" 																		   << '\n';
cout << "      		}" 																								<< '\n';
cout << "    	</style>" 																							<< '\n';
cout << "	</head>" 																								<< '\n';
cout << "	<body>" 																								<< '\n';
cout << "		<table class=\"table table-dark table-bordered\">" 													<< '\n';
cout << "			<thead>" 																						<< '\n';
cout << "				<tr>" 																						<< '\n';
for(unsigned int i = 0; i < shellServer.size(); i++){
	cout << "          		<th scope=\"col\">" << shellServer[i].host << ":" << shellServer[i].port << "</th>"		<< '\n';
}
cout << "        		</tr>" 																						<< '\n';
cout << "			</thead>" 																						<< '\n';
cout << "			<tbody>"																						<< '\n';
cout << "				<tr>" 																						<< '\n';
for(unsigned int i = 0; i < shellServer.size(); i++){
	cout << "          		<td><pre id=\"s" << i << "\" class=\"mb-0\"></pre></td>" 								<< '\n';
}
cout << "				</tr>" 																						<< '\n';
cout << "			</tbody>" 																						<< '\n';
cout << "		</table>" 																							<< '\n';
cout << "	</body>" 																								<< '\n';
cout << "</html>" 																									<< '\n';
}

int main(int argc, char* argv[]){
	try{
		cout << "Content-type: text/html\r\n\r\n" << flush;

		for(auto e : envSet){ env[e] = string(getenv(e.c_str())); }
		getShellServerInfo();
		http();

		boost::asio::io_context io_context;
		for(unsigned int i = 0; i < shellServer.size(); i++){ std::make_shared<ShellClient>(io_context, i)->start(); }
		HelloTimer hello_timer(io_context, shellServer.size());
		hello_timer.start();
		io_context.run();
	}catch (std::exception& e){
		std::cerr << "Exception: " << e.what() << "\n";
	}
	return 0;
}