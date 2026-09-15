#include "em/network.hpp"

SeenSet::SeenSet(size_t c):cap(c) {
}

bool SeenSet::contains(const std::string&x) {
    return s.count(x);
}

void SeenSet::add(const std::string&x) {
    if(s.count(x)) return;
    s.insert(x);
    q.push_back(x);
    if(q.size()>cap) {
        s.erase(q.front());
        q.pop_front();
    }
}

#ifdef _WIN32
static bool em_set_socket_timeout(em_socket_t fd,int optname,int seconds) {
    timeval tv{seconds,0};
    return setsockopt(
        fd,
        SOL_SOCKET,
        optname,
        reinterpret_cast<const char*>(&tv),
        static_cast<int>(sizeof(tv))
    ) == 0;
}

static bool em_set_socket_int(em_socket_t fd,int optname,int value) {
    return setsockopt(
        fd,
        SOL_SOCKET,
        optname,
        reinterpret_cast<const char*>(&value),
        static_cast<int>(sizeof(value))
    ) == 0;
}
#else
static bool em_set_socket_timeout(em_socket_t fd,int optname,int seconds) {
    timeval tv{seconds,0};
    return setsockopt(
        fd,
        SOL_SOCKET,
        optname,
        &tv,
        sizeof(tv)
    ) == 0;
}

static bool em_set_socket_int(em_socket_t fd,int optname,int value) {
    return setsockopt(
        fd,
        SOL_SOCKET,
        optname,
        &value,
        sizeof(value)
    ) == 0;
}
#endif

bool P2PNode::valid_hello(json_object*o) {
    std::string net;
    int pv=0,ppv=0;

    return
        json_get_string(o,"network_id",net) &&
        json_get_int(o,"protocol_version",pv) &&
        json_get_int(o,"p2p_version",ppv) &&
        net==NETWORK_ID &&
        pv==PROTOCOL_VERSION &&
        ppv==P2P_PROTOCOL_VERSION;
}

std::string P2PNode::msg(
    const std::string&type,
    const std::string&payload
) {
    return payload.empty() ?
        "{\"type\":"+json_escape(type)+"}" :
        "{\"type\":"+json_escape(type)+
        ",\"payload\":"+json_escape(payload)+"}";
}

std::string P2PNode::msg_num(
    const std::string&type,
    const std::string&payload
) {
    return "{\"type\":"+json_escape(type)+
           ",\"payload\":"+payload+"}";
}

void P2PNode::remember_peer(
    const std::string&host,
    int port
) {
    if(port<1 || port>65535 || host.empty())
        return;

    std::lock_guard<std::mutex>g(peers_mu);

    std::string k=host+":"+std::to_string(port);

    if(peers.size()<MAX_PEERS || peers.count(k))
        peers[k]={host,port};
}

bool P2PNode::send_blocks(
    em_socket_t fd,
    size_t start
) {
    auto c=bc.snapshot_from(
        start,
        MAX_BLOCKS_RESPONSE
    );

    std::string cj=chain_json(c);

    if(cj.empty())
        return send_frame(
            fd,
            msg("error","blocks_too_large")
        );

    return send_frame(
        fd,
        msg("blocks",cj)
    );
}

std::vector<std::string> P2PNode::build_locator(
    const std::vector<Block>&chain
) const {
    std::vector<std::string>locator;

    if(chain.empty())
        return locator;

    /*
     * Bitcoin-style locator:
     * - start from the current tip
     * - walk backwards with exponentially increasing steps
     * - always include genesis
     */
    size_t index=chain.size()-1;
    size_t step=1;

    while(true) {
        if(locator.empty() ||
           locator.back()!=chain[index].block_hash) {
            locator.push_back(chain[index].block_hash);
        }

        if(index==0)
            break;

        if(locator.size()>=MAX_LOCATOR-1) {
            locator.push_back(chain[0].block_hash);
            break;
        }

        size_t next;

        if(index>step)
            next=index-step;
        else
            next=0;

        index=next;

        if(step<chain.size())
            step*=2;
        else
            step=chain.size();
    }

    if(locator.empty() ||
       locator.back()!=chain[0].block_hash) {
        locator.push_back(chain[0].block_hash);
    }

    if(locator.size()>MAX_LOCATOR)
        locator.resize(MAX_LOCATOR);

    return locator;
}

bool P2PNode::request_headers(
    em_socket_t fd,
    const std::vector<std::string>&locator,
    int&common_index
) {
    common_index=-1;

    /*
     * Serialize locator as a JSON array.
     */
    std::ostringstream arr;
    arr<<"[";

    for(size_t i=0;i<locator.size();++i) {
        if(i)
            arr<<",";

        arr<<json_escape(locator[i]);
    }

    arr<<"]";

    std::string req=
        "{\"type\":\"getheaders\",\"locator\":"+arr.str()+"}";

    if(!send_frame(fd,req))
        return false;

    auto r=recv_frame(fd);

    if(!r)
        return false;

    json_object*o=json_tokener_parse(r->c_str());

    if(!o ||
       !json_object_is_type(o,json_type_object)) {
        if(o)
            json_object_put(o);

        return false;
    }

    std::string type;
    json_get_string(o,"type",type);

    if(type!="headers") {
        json_object_put(o);
        return false;
    }

    json_object*payload=nullptr;

    if(!json_object_object_get_ex(
        o,
        "payload",
        &payload
    ) ||
       !json_object_is_type(
           payload,
           json_type_string
       )) {
        json_object_put(o);
        return false;
    }

    const char*raw=json_object_get_string(payload);

    if(!raw) {
        json_object_put(o);
        return false;
    }

    json_object*po=json_tokener_parse(raw);

    if(!po ||
       !json_object_is_type(
           po,
           json_type_object
       )) {
        if(po)
            json_object_put(po);

        json_object_put(o);
        return false;
    }

    json_object*ci=nullptr;

    if(!json_object_object_get_ex(
        po,
        "common_index",
        &ci
    ) ||
       !json_object_is_type(
           ci,
           json_type_int
       )) {
        json_object_put(po);
        json_object_put(o);
        return false;
    }

    i64 v=json_object_get_int64(ci);

    if(v<0 || v>INT_MAX) {
        json_object_put(po);
        json_object_put(o);
        return false;
    }

    common_index=static_cast<int>(v);

    json_object_put(po);
    json_object_put(o);

    return true;
}

void P2PNode::handle(
    em_socket_t fd,
    std::string remote_host,
    int remote_port
) {
    em_set_socket_timeout(
        fd,
        SO_RCVTIMEO,
        P2P_HANDSHAKE_TIMEOUT_SEC
    );

    em_set_socket_timeout(
        fd,
        SO_SNDTIMEO,
        P2P_HANDSHAKE_TIMEOUT_SEC
    );

    auto h=recv_frame(fd);

    if(!h)
        return;

    json_object*ho=json_tokener_parse(
        h->c_str()
    );

    if(!ho || !valid_hello(ho)) {
        if(ho)
            json_object_put(ho);

        return;
    }

    json_object_put(ho);

    remember_peer(
        remote_host,
        remote_port
    );

    std::string hello=
        "{\"type\":\"hello\",\"network_id\":"+
        json_escape(NETWORK_ID)+
        ",\"protocol_version\":"+
        std::to_string(PROTOCOL_VERSION)+
        ",\"p2p_version\":"+
        std::to_string(P2P_PROTOCOL_VERSION)+
        "}";

    if(!send_frame(fd,hello))
        return;

    em_set_socket_timeout(
        fd,
        SO_RCVTIMEO,
        P2P_IDLE_TIMEOUT_SEC
    );

    for(;;) {
        auto fr=recv_frame(fd);

        if(!fr)
            break;

        json_object*o=json_tokener_parse(
            fr->c_str()
        );

        if(!o ||
           !json_object_is_type(
               o,
               json_type_object
           )) {
            if(o)
                json_object_put(o);

            break;
        }

        std::string type;
        std::string payload;

        json_get_string(
            o,
            "type",
            type
        );

        json_get_string(
            o,
            "payload",
            payload
        );

        json_object*po=nullptr;

        size_t start=0;

        if(json_object_object_get_ex(
            o,
            "start",
            &po
        ) &&
           json_object_is_type(
               po,
               json_type_int
           )) {
            i64 v=json_object_get_int64(po);

            if(v>=0)
                start=static_cast<size_t>(v);
        }

        /*
         * New common-ancestor locator.
         */
        std::vector<std::string>locator;

        json_object*lo=nullptr;

        if(json_object_object_get_ex(
            o,
            "locator",
            &lo
        ) &&
           json_object_is_type(
               lo,
               json_type_array
           )) {

            size_t n=
                json_object_array_length(lo);

            if(n>MAX_LOCATOR)
                n=MAX_LOCATOR;

            for(size_t i=0;i<n;++i) {
                json_object*item=
                    json_object_array_get_idx(
                        lo,
                        static_cast<int>(i)
                    );

                if(!item ||
                   !json_object_is_type(
                       item,
                       json_type_string
                   ))
                    continue;

                const char*hs=
                    json_object_get_string(item);

                if(hs &&
                   std::strlen(hs)==128) {
                    locator.emplace_back(hs);
                }
            }
        }

        json_object_put(o);

        if(type=="gettip") {

            auto c=bc.snapshot_chain();

            if(c.empty())
                break;

            if(!send_frame(
                fd,
                msg("tip",c.back().json())
            ))
                break;
        }

        else if(type=="getheaders") {

            auto c=bc.snapshot_chain();

            if(c.empty())
                break;

            int common=-1;

            /*
             * If a locator was supplied, search for the
             * first matching block on our chain.
             */
            if(!locator.empty()) {

                for(const auto&hash:locator) {

                    for(size_t i=c.size();i>0;--i) {

                        size_t index=i-1;

                        if(c[index].block_hash==hash) {
                            common=
                                static_cast<int>(
                                    index
                                );

                            break;
                        }
                    }

                    if(common>=0)
                        break;
                }
            }

            /*
             * Legacy start-based request is retained
             * for compatibility with the old protocol.
             */
            if(locator.empty()) {

                size_t from=
                    std::min(
                        start,
                        c.size()
                    );

                size_t end=
                    std::min(
                        c.size(),
                        from+MAX_BLOCKS_RESPONSE
                    );

                std::ostringstream out;

                out<<"{";
                out<<"\"common_index\":"
                   <<(from==0 ? 0 :
                      static_cast<int>(from-1))
                   <<",\"headers\":[";

                for(size_t i=from;i<end;++i) {
                    if(i>from)
                        out<<",";

                    out<<json_escape(
                        c[i].block_hash
                    );
                }

                out<<"]}";

                if(!send_frame(
                    fd,
                    msg("headers",out.str())
                ))
                    break;

                continue;
            }

            /*
             * No common ancestor means the peer is not
             * on this network's current genesis chain.
             */
            if(common<0) {

                if(!send_frame(
                    fd,
                    msg("error","no_common_ancestor")
                ))
                    break;

                continue;
            }

            size_t from=
                static_cast<size_t>(
                    common+1
                );

            size_t end=
                std::min(
                    c.size(),
                    from+MAX_BLOCKS_RESPONSE
                );

            std::ostringstream out;

            out<<"{";
            out<<"\"common_index\":"
               <<common
               <<",\"headers\":[";

            for(size_t i=from;i<end;++i) {
                if(i>from)
                    out<<",";

                out<<json_escape(
                    c[i].block_hash
                );
            }

            out<<"]}";

            if(!send_frame(
                fd,
                msg("headers",out.str())
            ))
                break;
        }

        else if(type=="getblocks") {

            if(!send_blocks(
                fd,
                start
            ))
                break;
        }

        else if(type=="getchain") {

            auto c=bc.snapshot_chain();

            std::string cj=chain_json(c);

            if(cj.empty()) {
                send_frame(
                    fd,
                    msg("error","chain_too_large")
                );

                break;
            }

            if(!send_frame(
                fd,
                msg("chain",cj)
            ))
                break;
        }

        else if(type=="ping") {

            if(!send_frame(
                fd,
                msg("pong")
            ))
                break;
        }

        else if(type=="submitblock") {

            Block b;

            bool ok=
                parse_block(payload,b) &&
                bc.accept_block(b);

            if(!send_frame(
                fd,
                msg(
                    ok ?
                    "accepted" :
                    "rejected"
                )
            ))
                break;
        }

        else if(type=="bye") {
            break;
        }

        else {

            if(!send_frame(
                fd,
                msg(
                    "error",
                    "unknown_message"
                )
            ))
                break;
        }
    }
}

em_socket_t P2PNode::connect_peer() {

    addrinfo hints{},*res=nullptr;

    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;

    std::string ps=
        std::to_string(peer_port);

    if(getaddrinfo(
        peer_host.c_str(),
        ps.c_str(),
        &hints,
        &res
    )!=0)
        return EM_INVALID_SOCKET;

    em_socket_t fd=
        EM_INVALID_SOCKET;

    for(addrinfo*r=res;r;r=r->ai_next) {

        fd=socket(
            r->ai_family,
            r->ai_socktype,
            r->ai_protocol
        );

        if(fd==EM_INVALID_SOCKET)
            continue;

        if(connect(
            fd,
            r->ai_addr,
            r->ai_addrlen
        )==0)
            break;

        em_socket_close(fd);

        fd=EM_INVALID_SOCKET;
    }

    freeaddrinfo(res);

    return fd;
}

bool P2PNode::handshake(
    em_socket_t fd
) {

    std::string h=
        "{\"type\":\"hello\",\"network_id\":"+
        json_escape(NETWORK_ID)+
        ",\"protocol_version\":"+
        std::to_string(PROTOCOL_VERSION)+
        ",\"p2p_version\":"+
        std::to_string(P2P_PROTOCOL_VERSION)+
        "}";

    if(!send_frame(fd,h))
        return false;

    auto r=recv_frame(fd);

    if(!r)
        return false;

    json_object*o=
        json_tokener_parse(
            r->c_str()
        );

    bool ok=false;

    if(o) {

        std::string type;
        std::string net;

        int pv=0;
        int ppv=0;

        ok=
            json_get_string(
                o,
                "type",
                type
            ) &&
            type=="hello" &&
            json_get_string(
                o,
                "network_id",
                net
            ) &&
            json_get_int(
                o,
                "protocol_version",
                pv
            ) &&
            json_get_int(
                o,
                "p2p_version",
                ppv
            ) &&
            net==NETWORK_ID &&
            pv==PROTOCOL_VERSION &&
            ppv==P2P_PROTOCOL_VERSION;

        json_object_put(o);
    }

    return ok;
}

bool P2PNode::request_range(
    em_socket_t fd,
    size_t start,
    std::vector<Block>&out
) {

    std::string req=
        "{\"type\":\"getblocks\",\"start\":"+
        std::to_string(start)+
        "}";

    if(!send_frame(fd,req))
        return false;

    auto r=recv_frame(fd);

    if(!r)
        return false;

    json_object*o=
        json_tokener_parse(
            r->c_str()
        );

    if(!o)
        return false;

    std::string type;
    std::string payload;

    json_get_string(
        o,
        "type",
        type
    );

    json_get_string(
        o,
        "payload",
        payload
    );

    json_object_put(o);

    if(type!="blocks")
        return false;

    std::vector<Block>v;

    if(!parse_chain(
        payload,
        v
    ))
        return false;

    out.insert(
        out.end(),
        v.begin(),
        v.end()
    );

    return true;
}

void P2PNode::sync_once() {

    em_socket_t fd=
        connect_peer();

    if(fd==EM_INVALID_SOCKET)
        return;

    em_set_socket_timeout(
        fd,
        SO_RCVTIMEO,
        P2P_HANDSHAKE_TIMEOUT_SEC
    );

    em_set_socket_timeout(
        fd,
        SO_SNDTIMEO,
        P2P_HANDSHAKE_TIMEOUT_SEC
    );

    if(!handshake(fd)) {

        em_socket_close(fd);

        return;
    }

    auto local=
        bc.snapshot_chain();

    if(local.empty()) {

        send_frame(
            fd,
            msg("bye")
        );

        em_socket_close(fd);

        return;
    }

    /*
     * Step 1:
     * build a locator from our chain and ask the peer
     * to identify the common ancestor.
     */
    auto locator=
        build_locator(local);

    int common_index=-1;

    if(!request_headers(
        fd,
        locator,
        common_index
    )) {

        send_frame(
            fd,
            msg("bye")
        );

        em_socket_close(fd);

        return;
    }

    /*
     * The peer may have no common ancestor. In a valid
     * network this should not happen because genesis and
     * network parameters are fixed.
     */
    if(common_index<0 ||
       common_index>=static_cast<int>(local.size())) {

        send_frame(
            fd,
            msg("bye")
        );

        em_socket_close(fd);

        return;
    }

    /*
     * If the common ancestor is our current tip, that does NOT
     * mean that the peer has no newer blocks. It is the normal
     * catch-up case when our node is simply behind the peer.
     *
     * Continue with start = common_index + 1. If the peer really
     * has no newer blocks, request_range() will return an empty
     * response and we will exit normally below.
     */

    /*
     * Step 2:
     * download the peer's suffix beginning immediately
     * after the common ancestor.
     */
    size_t start=
        static_cast<size_t>(
            common_index+1
        );

    std::vector<Block>remote;

    while(true) {

        std::vector<Block>chunk;

        if(!request_range(
            fd,
            start,
            chunk
        ))
            break;

        if(chunk.empty())
            break;

        remote.insert(
            remote.end(),
            chunk.begin(),
            chunk.end()
        );

        start+=chunk.size();

        if(chunk.size()<
           MAX_BLOCKS_RESPONSE)
            break;
    }

    if(!remote.empty()) {

        /*
         * Keep our chain through the common ancestor,
         * then append the peer's fork.
         */
        std::vector<Block>candidate;

        candidate.insert(
            candidate.end(),
            local.begin(),
            local.begin()+
                common_index+1
        );

        candidate.insert(
            candidate.end(),
            remote.begin(),
            remote.end()
        );

        /*
         * replace_chain() performs full validation and
         * accepts the candidate only when its cumulative
         * work is greater than the current chain.
         */
        bc.replace_chain(candidate);
    }

    send_frame(
        fd,
        msg("bye")
    );

    em_socket_close(fd);
}

P2PNode::P2PNode(
    Blockchain&b,
    int lp
):
    bc(b),
    listen_port(lp) {
    em_socket_init();
}

P2PNode::P2PNode(
    Blockchain&b,
    const std::string&h,
    int p
):
    bc(b),
    peer_host(h),
    peer_port(p) {
    em_socket_init();
}

void P2PNode::serve() {

    em_socket_init();

    server_fd=
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if(server_fd==EM_INVALID_SOCKET)
        throw std::runtime_error(
            "P2P socket failed"
        );

    if(!em_set_socket_int(
        server_fd,
        SO_REUSEADDR,
        1
    )) {

        em_socket_close(server_fd);

        server_fd=
            EM_INVALID_SOCKET;

        throw std::runtime_error(
            "P2P setsockopt failed"
        );
    }

    sockaddr_in a{};

    a.sin_family=AF_INET;
    a.sin_addr.s_addr=
        htonl(INADDR_ANY);

    a.sin_port=
        htons(
            static_cast<uint16_t>(
                listen_port
            )
        );

    if(bind(
        server_fd,
        (sockaddr*)&a,
        sizeof(a)
    )<0 ||
       listen(
           server_fd,
           MAX_PEERS
       )<0) {

        em_socket_close(server_fd);

        server_fd=
            EM_INVALID_SOCKET;

        throw std::runtime_error(
            "P2P bind/listen failed"
        );
    }

    std::cout
        <<"[P2P] listening on 0.0.0.0:"
        <<listen_port
        <<"\n";

    while(!stop) {

        sockaddr_storage ss{};

#ifdef _WIN32
        int n=sizeof(ss);
#else
        socklen_t n=sizeof(ss);
#endif

        em_socket_t fd=
            accept(
                server_fd,
                (sockaddr*)&ss,
                &n
            );

        if(fd==EM_INVALID_SOCKET)
            break;

        std::string rh="peer";
        int rp=0;

        if(ss.ss_family==AF_INET) {

            auto*a=
                (sockaddr_in*)&ss;

            char buf[INET_ADDRSTRLEN];

            if(inet_ntop(
                AF_INET,
                &a->sin_addr,
                buf,
                sizeof(buf)
            ))
                rh=buf;

            rp=ntohs(a->sin_port);
        }

        std::thread(
            [this,fd,rh,rp]{
                handle(
                    fd,
                    rh,
                    rp
                );

                em_socket_close(fd);
            }
        ).detach();
    }
}

void P2PNode::sync_loop() {

    while(!stop) {

        sync_once();

        for(
            int i=0;
            i<15 && !stop;
            ++i
        )
            std::this_thread::sleep_for(
                std::chrono::seconds(1)
            );
    }
}

bool send_all(
    em_socket_t fd,
    const char*p,
    size_t n
) {

    while(n) {

        const int chunk=
            static_cast<int>(
                std::min<size_t>(
                    n,
                    static_cast<size_t>(
                        INT_MAX
                    )
                )
            );

        const int r=
            send(
                fd,
                p,
                chunk,
                0
            );

        if(r<=0)
            return false;

        p+=r;
        n-=r;
    }

    return true;
}

bool recv_all(
    em_socket_t fd,
    char*p,
    size_t n
) {

    while(n) {

        const int chunk=
            static_cast<int>(
                std::min<size_t>(
                    n,
                    static_cast<size_t>(
                        INT_MAX
                    )
                )
            );

        const int r=
            recv(
                fd,
                p,
                chunk,
                0
            );

        if(r<=0)
            return false;

        p+=r;
        n-=r;
    }

    return true;
}

bool send_frame(
    em_socket_t fd,
    const std::string&s
) {

    if(s.size()>MAX_FRAME_BYTES)
        return false;

    uint32_t n=
        htonl(
            static_cast<uint32_t>(
                s.size()
            )
        );

    return
        send_all(
            fd,
            (char*)&n,
            4
        ) &&
        send_all(
            fd,
            s.data(),
            s.size()
        );
}

std::optional<std::string> recv_frame(
    em_socket_t fd
) {

    uint32_t n=0;

    if(!recv_all(
        fd,
        (char*)&n,
        4
    ))
        return std::nullopt;

    n=ntohl(n);

    if(
        n==0 ||
        n>MAX_FRAME_BYTES
    )
        return std::nullopt;

    std::string s(
        n,
        '\0'
    );

    if(!recv_all(
        fd,
        s.data(),
        n
    ))
        return std::nullopt;

    return s;
}
