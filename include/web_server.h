#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <microhttpd.h>

struct MHD_Daemon *web_server_start_https(
    unsigned short port,
    const char *index_file,
    const char *certificate_file,
    const char *private_key_file
);

struct MHD_Daemon *web_server_start_redirect(
    unsigned short http_port,
    unsigned short https_port
);

void web_server_stop(
    struct MHD_Daemon *daemon
);

void web_server_tls_cleanup(void);

#endif
