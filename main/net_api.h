#pragma once

#include <stddef.h>

// Bring up Wi-Fi STA, mDNS (<hostname>.local) and the token-authenticated
// HTTP API. Returns after the server is listening; Wi-Fi connects async.
void net_api_start(void);

// Current STA IPv4 as a string ("0.0.0.0" before association).
void net_api_ip(char *buf, size_t n);
