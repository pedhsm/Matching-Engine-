// zmq_smoke.cpp
//
// Build-foundation smoke test: proves that libzmq + the cppzmq header-only
// binding compile, LINK and RUN on this machine. It does NOT implement any
// matching / parsing / networking logic -- it only exercises the ZMQ types the
// engine will rely on (context + socket construction) and reports the linked
// libzmq version. No real bind/connect is performed.

#include <zmq.hpp>
#include <cstdio>

int main() {
    // Linked libzmq version (from the actual shared/static lib we linked).
    int major = 0, minor = 0, patch = 0;
    zmq_version(&major, &minor, &patch);
    std::printf("libzmq version: %d.%d.%d\n", major, minor, patch);

    // cppzmq (header) version.
    std::printf("cppzmq version: %d.%d.%d\n",
                CPPZMQ_VERSION_MAJOR, CPPZMQ_VERSION_MINOR, CPPZMQ_VERSION_PATCH);

    // Exercise the core cppzmq objects the engine will use. Constructing a
    // context spins up libzmq's I/O threads; constructing a socket allocates a
    // real 0MQ socket. This is enough to force the linker to pull in libzmq.
    zmq::context_t context{1};
    zmq::socket_t  pub_socket{context, zmq::socket_type::pub};
    zmq::socket_t  pull_socket{context, zmq::socket_type::pull};

    // Touch the sockets so the optimizer cannot elide them.
    std::printf("sockets valid: pub=%d pull=%d\n",
                static_cast<bool>(pub_socket) ? 1 : 0,
                static_cast<bool>(pull_socket) ? 1 : 0);

    std::printf("ZMQ OK\n");
    return 0;
}
