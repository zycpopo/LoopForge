#ifndef TCP_SERVER_NETWORK_HPP
#define TCP_SERVER_NETWORK_HPP

#include "server_common.hpp"

class NetWork {
    public:
        NetWork() {
            DBG_LOG("SIGPIPE INIT");
            signal(SIGPIPE, SIG_IGN);
        }
};
static NetWork nw;

#endif


