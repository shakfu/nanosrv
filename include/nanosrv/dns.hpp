#pragma once
#include "net.hpp"

#define MG_DNS_RTYPE_A 1
#define MG_DNS_RTYPE_PTR 12
#define MG_DNS_RTYPE_TXT 16
#define MG_DNS_RTYPE_AAAA 28
#define MG_DNS_RTYPE_SRV 33

namespace nanosrv {
// DNS types and functions are declared in net.hpp since Mgr depends on them
// This header is provided for organizational clarity
} // namespace nanosrv
