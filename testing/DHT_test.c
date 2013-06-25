/* DHT test
 * A file with a main that runs our DHT for testing.
 * 
 * Compile with: gcc -Wall -o test ../core/DHT.c DHT_test.c
 * 
 * Command line arguments are the ip and port of a node
 * EX: ./test 127.0.0.1 33445
 */

#include "../core/DHT.h"

#include <string.h>

//Sleep function (x = milliseconds)
#ifdef WIN32

#define c_sleep(x) Sleep(1*x)

#else
#include <unistd.h>
#include <arpa/inet.h>
#define c_sleep(x) usleep(1000*x)

#endif

#define PORT 33445


void print_clientlist()
{
    uint32_t i, j;
    IP_Port p_ip;
    printf("___________________CLOSE________________________________\n");
    for(i = 0; i < 4; i++)
    {
        printf("ClientID: ");
        for(j = 0; j < 32; j++)
        {
            printf("%c", close_clientlist[i].client_id[j]);
        }
        p_ip = close_clientlist[i].ip_port;
        printf("\nIP: %u.%u.%u.%u Port: %u",p_ip.ip.c[0],p_ip.ip.c[1],p_ip.ip.c[2],p_ip.ip.c[3],ntohs(p_ip.port));
        printf("\nTimestamp: %u\n", close_clientlist[i].timestamp);
    }  
}

void print_friendlist()
{
    uint32_t i, j, k;
    IP_Port p_ip;
    printf("_________________FRIENDS__________________________________\n");
    for(k = 0; k < num_friends; k++)
    {
        printf("FRIEND %u\n", k);
        printf("ID: ");
        for(j = 0; j < 32; j++)
        {
            printf("%c", friends_list[k].client_id[j]);
        }
        p_ip = getfriendip(friends_list[k].client_id);
        printf("\nIP: %u.%u.%u.%u:%u",p_ip.ip.c[0],p_ip.ip.c[1],p_ip.ip.c[2],p_ip.ip.c[3],ntohs(p_ip.port));

        printf("\nCLIENTS IN LIST:\n\n ");
        
        for(i = 0; i < 4; i++)
        {
            printf("ClientID: ");
            for(j = 0; j < 32; j++)
            {
                printf("%X", friends_list[k].client_list[i].client_id[j]);
            }
            p_ip = friends_list[k].client_list[i].ip_port;
            printf("\nIP: %u.%u.%u.%u:%u",p_ip.ip.c[0],p_ip.ip.c[1],p_ip.ip.c[2],p_ip.ip.c[3],ntohs(p_ip.port));
            printf("\nTimestamp: %u\n", friends_list[k].client_list[i].timestamp);
        }
    }
}



int main(int argc, char *argv[])
{
    srand(time(NULL));
    int randdomnum = rand();
    memcpy(self_client_id, &randdomnum, 4);
    
    #ifdef WIN32
    WSADATA wsaData;
    if(WSAStartup(MAKEWORD(2,2), &wsaData) != NO_ERROR)
    {
        return -1;
    }
    #endif
    
    if (argc < 4) {
        printf("usage %s ip port client_id\n", argv[0]);
        exit(0);
    }
    addfriend(argv[3]);
    
    //initialize our socket
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); 
    //Set socket nonblocking
    #ifdef WIN32
    //I think this works for windows
    u_long mode = 1;
    //ioctl(sock, FIONBIO, &mode);
    ioctlsocket(sock, FIONBIO, &mode); 
    #else
    fcntl(sock, F_SETFL, O_NONBLOCK, 1);
    #endif
    
    //Bind our socket to port PORT and address 0.0.0.0
    ADDR addr = {AF_INET, htons(PORT), {{0}}}; 
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    perror("Initialization");
    IP_Port bootstrap_ip_port;
    bootstrap_ip_port.port = htons(atoi(argv[2]));
    //bootstrap_ip_port.ip.c[0] = 127;
    //bootstrap_ip_port.ip.c[1] = 0;
    //bootstrap_ip_port.ip.c[2] = 0;
    //bootstrap_ip_port.ip.c[3] = 1;
    bootstrap_ip_port.ip.i = inet_addr(argv[1]);
    bootstrap(bootstrap_ip_port);
    
    IP_Port ip_port;
    char data[MAX_UDP_PACKET_SIZE];
    uint32_t length;
    
    uint32_t i;
    
    while(1)
    {
            
        doDHT();
        
        while(recievepacket(&ip_port, data, &length) != -1)
        {
            if(DHT_recvpacket(data, length, ip_port))
            {
                printf("UNHANDLED PACKET RECEIVED\nLENGTH:%u\nCONTENTS:\n", length);
                printf("--------------------BEGIN-----------------------------\n");
                for(i = 0; i < length; i++)
                {
                    if(data[i] < 16)
                        printf("0");
                    printf("%X",data[i]);
                }
                printf("\n--------------------END-----------------------------\n\n\n");
            }
            else
            {
                printf("Received handled packet with length: %u\n", length);
            }
        }
        print_clientlist();
        print_friendlist();
        c_sleep(300);
    }
    
    #ifdef WIN32
    WSACleanup();
    #endif
    return 0;   
}