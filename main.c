#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>


void readSettingsFile() {
}

int main(int argc, char *argv[]) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    
    
    char* port = "8080";
    struct addrinfo* addresses = getAvailibleAddresses(&hints, port);
    return 0;
}
