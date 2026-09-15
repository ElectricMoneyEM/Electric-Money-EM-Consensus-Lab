#pragma once
#include "common.hpp"
#include "block.hpp"
#include "blockchain.hpp"

class SeenSet {
    size_t cap;
    std::set<std::string>s;
    std::deque<std::string>q;
    explicit SeenSet(size_t c);
    bool contains(const std::string&x);
    void add(const std::string&x);
};

bool send_all(em_socket_t fd,const char*p,size_t n);

bool recv_all(em_socket_t fd,char*p,size_t n);

bool send_frame(em_socket_t fd,const std::string&s);

std::optional<std::string> recv_frame(em_socket_t fd);

class P2PNode {
    Blockchain&bc;
    int listen_port=0;
    std::string peer_host;
    int peer_port=0;
    std::atomic<bool>stop{false};
    em_socket_t server_fd=EM_INVALID_SOCKET;

    std::mutex peers_mu;
    std::map<std::string,std::pair<std::string,int>> peers;

    static constexpr size_t MAX_BLOCKS_RESPONSE=128;
    static constexpr size_t MAX_LOCATOR=32;

    static bool valid_hello(json_object*o);

    static std::string msg(
        const std::string&type,
        const std::string&payload=""
    );

    static std::string msg_num(
        const std::string&type,
        const std::string&payload
    );

    void remember_peer(
        const std::string&host,
        int port
    );

    bool send_blocks(
        em_socket_t fd,
        size_t start
    );

    void handle(
        em_socket_t fd,
        std::string remote_host,
        int remote_port
    );

    em_socket_t connect_peer();

    bool handshake(em_socket_t fd);

    std::vector<std::string> build_locator(
        const std::vector<Block>&chain
    ) const;

    bool request_headers(
        em_socket_t fd,
        const std::vector<std::string>&locator,
        int&common_index
    );

    bool request_range(
        em_socket_t fd,
        size_t start,
        std::vector<Block>&out
    );

    void sync_once();

    P2PNode(
        Blockchain&b,
        int lp
    );

    P2PNode(
        Blockchain&b,
        const std::string&h,
        int p
    );

    void serve();

    void sync_loop();
};
