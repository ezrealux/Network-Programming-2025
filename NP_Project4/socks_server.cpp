// socks_server.cpp

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/algorithm/string/replace.hpp>

#include <iostream>
#include <fstream>

#include <regex>
#include <string.h>
#include <map>
#include <vector>

#include <unistd.h>     // for fork
#include <arpa/inet.h>  // for inet_ntoa
#include <netinet/in.h>
#include <sys/wait.h>

using tcp = boost::asio::ip::tcp;
using namespace std;

//----------------------------------------------------------------------------------------------------------------------
// OBJECTIVES
//----------------------------------------------------------------------------------------------------------------------

struct socks4Info{
	int vn;
	int cd;
	string srcIP;
	string srcPort;
	string dstIP;
	string dstPort;
	string cmd;
	string reply;
};

class Session : public enable_shared_from_this<Session>{
    public:
    Session(tcp::socket socket, boost::asio::io_context& io_context)
     : ClientSocket_(move(socket)), DstSocket_(io_context), io_context_(io_context), acceptor_(io_context){}

    void start() { do_read_request(); }
    
    private:
        struct socks4Info request;
        enum { max_length = 10240 };
        char data_[max_length];
        uint8_t reply[8];
        tcp::socket ClientSocket_;
        tcp::socket DstSocket_;
        boost::asio::io_context& io_context_;
        boost::asio::ip::tcp::acceptor acceptor_;

        void do_read_request();
        void do_write_reply();
        void do_connect();
        void do_bind();
        void do_bidirectional_traffic();
        void do_relay(tcp::socket& src, tcp::socket& dst);

        void parse_sock4(int length);
        void firewall();
        void fill_reply(tcp::endpoint remote_ep);
};

class Server {
    public:
    Server(boost::asio::io_context &io_context, uint16_t port)
     : io_context_(io_context), acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

    private:
    boost::asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    static inline int active_sessions = 0;

    static void handle_child_exit(int) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
            active_sessions--;  // ✅ 在 parent 收回資源後遞減
        }
    }

    void do_accept() {
        // if a SOCKS client connects, use fork() to tackle with it.
        acceptor_.async_accept(
            [this](boost::system::error_code, tcp::socket socket) {
                if (active_sessions >= 2) {
                    cerr << "[Server] Too many clients. Connection refused." << endl;
                    socket.close();
                    do_accept();
                    return;
                }

                // notify_fork(prepare/parent/child)
                // 告訴io_context: 要做fork了/是parent/是child
                io_context_.notify_fork(boost::asio::io_context::fork_prepare);
                pid_t child; // fork失敗，回收資源後重來
                while ((child = fork()) == -1) {
                    if (errno == EAGAIN) { wait(nullptr); }
                }

                if (child != 0) { // parent process
                    io_context_.notify_fork(boost::asio::io_context::fork_parent);
                    active_sessions++;
                    do_accept();
                    return;
                } else { // child
                    io_context_.notify_fork(boost::asio::io_context::fork_child);
                    acceptor_.close();
                    cout << "Active Session: " << active_sessions << endl;
                    make_shared<Session>(move(socket), io_context_)->start();
                }
            }
        );
    }
};

//----------------------------------------------------------------------------------------------------------------------
// SESSION FUNCTION
//----------------------------------------------------------------------------------------------------------------------

void Session::do_read_request() {
    auto self(shared_from_this());
    memset(data_, '\0', sizeof(data_));

    // 1. Receive SOCKS4 REQUEST from the SOCKS client
    ClientSocket_.async_read_some(
        boost::asio::buffer(data_, max_length), 
        [this, self](boost::system::error_code ec, size_t length) {
            if (!ec){
                // 2. Get the destination IP and port from SOCKS4 REQUEST
                parse_sock4(int(length));
                // 3. Check the firewall (socks.conf), and send SOCKS4 REPLY to the SOCKS client if rejected
                firewall();
                cout << "============== new request ===========" << endl;
                cout << "<S_IP>: " << request.srcIP << endl;
                cout << "<S_PORT>: " << request.srcPort << endl;
                cout << "<D_IP>: " << request.dstIP << endl;
                cout << "<D_PORT>: " << request.dstPort << endl;
                cout << "<Command>: " << request.cmd << endl;
                cout << "<Reply>: " << request.reply << endl;
                cout << "======================================" << endl;
                
                reply[0] = 0;
                if (request.reply == "Accept") {
                    // 4. Check CD value and choose one of the operations
                    reply[1] = 90;
                    if(request.cd == 1){ do_connect(); } 
                    else { do_bind(); }
                } else {
                    reply[1] = 91;
                    do_write_reply();
                    exit(0);
                }
            }
        }
    );
}

void Session::do_connect() {
    //cout << "============= Do resolve =============" << endl;
    auto self(shared_from_this());
    tcp::resolver resolver_(io_context_);
    try {
        tcp::resolver::results_type endpoint_ = resolver_.resolve(request.dstIP, request.dstPort);
        //for (const auto& ep : endpoint_) {
        //cout << "[Debug] resolved IP: " << ep.endpoint().address().to_string() << ", port: " << ep.endpoint().port() << endl;
        //}

        //cout << "============= Do connect =============" << endl;
        boost::asio::async_connect(
            DstSocket_, endpoint_,
            [this, self](boost::system::error_code ec, tcp::endpoint /*ed*/) {
                //cout << "=== Async_connect callback entered ===" << endl;

                if(!ec) {
                    // 2. Send SOCKS4 REPLY to the SOCKS client
                    //cout << "========= Connection success =========" << endl;
                    //for(int i=0; i < 8; i++) {cout << int(reply[i]) << " ";}
                    reply[1] = 90;
                    fill_reply(DstSocket_.remote_endpoint());          
                    do_write_reply();

                    // 3. Start relaying traffic (<->)
                    do_bidirectional_traffic();
                } else {;
                    cout << "========== Connection failed =========" << endl;
                    reply[1] = 91;
                    do_write_reply();
                    ClientSocket_.close();
                    DstSocket_.close();
                }
            }
        );
    } catch(const exception& e) {
        cout << "Resolution failed: " << e.what() << endl;
        reply[1] = 91;
        do_write_reply();
        ClientSocket_.close();
        DstSocket_.close();
    }
}

void Session::do_bind() {
    auto self(shared_from_this());
    // 1. Bind and listen a port
    acceptor_.open(tcp::v4());
    acceptor_.bind(tcp::endpoint(tcp::v4(), 0));
    acceptor_.listen();
    
    // 2. Send SOCKS4 REPLY to SOCKS client to tell which port to connect
    fill_reply(acceptor_.local_endpoint());
    do_write_reply();               
               
    // 3. Accept connection from destination and send another SOCKS4 REPLY to SOCKS client      
    acceptor_.async_accept(DstSocket_, 
        [this, self](boost::system::error_code ec) {
            if (!ec) {
                // 連線成功，發送第二個 reply
                fill_reply(DstSocket_.remote_endpoint());
                do_write_reply();

                // 4. Start relaying traffic on both directions
                do_bidirectional_traffic();
            } else {
                cerr << "Accept failed: " << ec.message() << endl;
                ClientSocket_.close();
                DstSocket_.close();
            }
        }
    );
}

void Session::do_write_reply() {
    auto self(shared_from_this());
    boost::asio::async_write(
        ClientSocket_, boost::asio::buffer(reply, 8),
        [this, self](boost::system::error_code ec, size_t /*length*/){
            if (!ec) {
                //cout << "[Debug] SOCKS4 REPLY sent to client" << endl;
            } else {
                cerr << "[Error] Failed to send SOCKS4 REPLY: " << ec.message() << endl;
                ClientSocket_.close();
            }
        }
    );
}

void Session::do_bidirectional_traffic() {
    do_relay(ClientSocket_, DstSocket_);
    do_relay(DstSocket_, ClientSocket_);
}

void Session::do_relay(tcp::socket& src, tcp::socket& dst) {
    auto self = shared_from_this();
    auto buffer = std::make_shared<std::array<char, max_length>>();
    
    src.async_read_some(boost::asio::buffer(*buffer), // read from src
        [this, self, buffer, &src, &dst](boost::system::error_code ec_r, size_t length) {
            if (!ec_r) {
                boost::asio::async_write(dst, // write to dst
                    boost::asio::buffer(*buffer, length),
                    [this, self, &src, &dst](boost::system::error_code ec_w, size_t /*length*/) {
                        if (!ec_w) { self->do_relay(src, dst);  // 繼續接收
                        } else { 
                            src.close();
                            dst.close();
                            exit(1);
                        }
                    }
                );
            } else { 
                src.close();
                dst.close();
                exit(1); 
            }
        }
    );
}

void Session::parse_sock4(int length) {
    request.reply = "Accept";
    for(int i = 2; i < 8; i++) reply[i] = 0;
    
    if (length < 9) { // sock4 request長度必>9
        request.reply = "Reject";
        return;
    }

    request.vn = data_[0];
    if(request.vn != 4) request.reply = "Reject";
    request.cd = data_[1];
    request.cmd = ((request.cd == 1) ? "CONNECT" : "BIND");

    int port_int = (static_cast<uint8_t>(data_[2]) << 8) | static_cast<uint8_t>(data_[3]);
    request.dstPort = to_string(port_int);
    
    if (data_[4] == 0 && data_[5] == 0 && data_[6] == 0 && data_[7] != 0) { // With domain name: BIND
        // DSTIP(variable) -> null(1) -> DOMAIN NAME(variable)
        int index = 8;
        while(data_[index] != 0) index++;
        index++;

        string domain = "";
        while(data_[index] != 0) domain.push_back(data_[index++]);
        tcp::resolver resolver_(io_context_);
        tcp::endpoint endpoint_ = resolver_.resolve(domain, request.dstPort)->endpoint();
        request.dstIP = endpoint_.address().to_string();
    } else { // ordinary ip: CONNECT
        request.dstIP = to_string(static_cast<uint8_t>(data_[4])) + "." +
                        to_string(static_cast<uint8_t>(data_[5])) + "." +
                        to_string(static_cast<uint8_t>(data_[6])) + "." +
                        to_string(static_cast<uint8_t>(data_[7]));
    }

    request.srcIP = ClientSocket_.remote_endpoint().address().to_string();
    request.srcPort = to_string(ClientSocket_.remote_endpoint().port());
}

void Session::firewall() {
    string socks_operation = request.cmd == "CONNECT" ?  "c" : "b";

    ifstream input("./socks.conf");
    string line;
    while (getline(input, line)) {
        istringstream iss(line);
        string permit, mode, rule_ip;
        iss >> permit >> mode >> rule_ip;

        if (mode != socks_operation) continue; // only check conf line of current operation

        istringstream rule_ss(rule_ip);
        istringstream ip_ss(request.dstIP);
        string rule_seg, ip_seg;
        for (int i = 0; i < 4; ++i) {
            if (!std::getline(rule_ss, rule_seg, '.') || !std::getline(ip_ss, ip_seg, '.')) request.reply = "Reject";
            if (rule_seg != "*" && rule_seg != ip_seg) request.reply = "Reject";
        }
    }
}

void Session::fill_reply(tcp::endpoint ep) {
    uint16_t port = ep.port();
    uint32_t ip = ep.address().to_v4().to_uint(); // Already in host byte order

    reply[2] = (port >> 8) & 0xFF;
    reply[3] = port & 0xFF;
    reply[4] = (ip >> 24) & 0xFF;
    reply[5] = (ip >> 16) & 0xFF;
    reply[6] = (ip >> 8) & 0xFF;
    reply[7] = (ip >> 0) & 0xFF;
    
    //cout << "Reply: ";
    //for(int i=0; i < 8; i++) {cout << int(reply[i]) << " ";}
    //cout << endl;
}

//----------------------------------------------------------------------------------------------------------------------
// MAIN FUNCTION
//----------------------------------------------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc != 2){
        cerr << "Usage: ./socks_server <port>\n";
        return 1;
    }

    try{
		boost::asio::io_context io_context;
		Server server(io_context, atoi(argv[1]));
		io_context.run();
	} catch (exception& e){
	    cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}