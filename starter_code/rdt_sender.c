#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "packet.h"
#include "common.h"

#define STDIN_FD    0
#define RETRY  120 //millisecond
#define PACKET_SIZE 9000000

int next_seqno=0;   //next sequence number to send
int send_base=0;    //the seqno of the first packet in the window
int window_size = 10; //window size
int arb_next_seqno =0;   //to track index of the next_seqno in the array of packets
int arb_baseno =0;   //to track index of the send_base in the array of packets
tcp_packet *packets[PACKET_SIZE]; //store all packets

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       


void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets ranging from arb_baseno and arb_next_seqno
        VLOG(INFO, "Timeout happened");
        int i;
        for (i = arb_baseno ; i < arb_next_seqno; i++)
        {
            VLOG(INFO, "Resend packet %d", i+1); //(packet index) +1 in array is printed out here not the actual byte sequence number
            sndpkt = packets[i];
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE+get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("Could not send packet\n");
            }
        }
    }
}

// start timer
void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

// stop timer
void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}


/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main (int argc, char **argv)
{
    int portno, len;        // port to listen on and length of address    
    char *hostname;         // host to send to
    char buffer[DATA_SIZE];     // buffer for reading data from stdin
    char ack_buffer[TCP_HDR_SIZE];  // buffer for receivin ACKs
    FILE *fp;           // file pointer for reading from file

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol

    init_timer(RETRY, resend_packets);      // initialize timer
    next_seqno = 0;             // initialize sequence number
    send_base = 0;              // initialize send base

    // Read size of file
    fseek(fp, 0L, SEEK_END);
    int file_size = ftell(fp);
    fseek(fp, 0L, SEEK_SET);
    
    // bzero packets
    bzero(packets, PACKET_SIZE);


    // Read file into packets and store in array
    int j = 0; // to track the number of packets added to packets array
    while (1) {
        // bzero buffer
        bzero(buffer, DATA_SIZE);
        // Read data from file
        len = fread(buffer, 1,DATA_SIZE, fp);
        if (len <0){
            len = 0;
        }
        // Create packet
        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);

        // Create sequence number based on bytes
        sndpkt->hdr.seqno= next_seqno;
        next_seqno += len;

        // Store packet in array
        packets[j] = sndpkt;

        VLOG(DEBUG, "Added packet %d at location %d in the packets array",
            sndpkt->hdr.seqno, j);

        j++;

        if(len == 0){
            break;
        }
    }

    // Close file
    fclose(fp);

    int i = 0; // to track the number of packets sent
    // Send first window of packets
    while (i < window_size && i < j) {
        // Send packet
        if(sendto(sockfd, packets[i], TCP_HDR_SIZE + get_data_size(packets[i]), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("Failed to send packet\n");
        }

        VLOG(INFO, "Sent packet %d", packets[i]->hdr.seqno);
        i++;
    }

    send_base = 0;
    next_seqno = packets[i]->hdr.seqno + get_data_size(packets[i]);
    // assign arb_next_seqno and arb_baseno
    arb_next_seqno = i;

    // Implement sliding window based on SendBackN protocol 
    while (1) {
        // Start timer
        start_timer();

        do{
            // bzero buffer
            bzero(ack_buffer, TCP_HDR_SIZE);
            // Receive ACK
            if(recvfrom(sockfd, ack_buffer, TCP_HDR_SIZE, 0, 
                        (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) < 0)
            {
                error("Failed to receive ACK\n");
            }
            recvpkt = (tcp_packet *)ack_buffer;
            assert(get_data_size(recvpkt) <= DATA_SIZE);

            VLOG(INFO, "Received ACK %d", recvpkt->hdr.ackno);

        } while (recvpkt->hdr.ackno <= send_base && send_base<file_size );    // Ignore old ACKs or corrupt ones
        
        // Stop timer
        stop_timer();
        
        // Update send base
        send_base = recvpkt->hdr.ackno;
        
         // Check if all packets have been sent
        if (i>=j && send_base >= file_size) {
            break;
        }


        // Send next packet if window is less than max window size
        if (i<j) {
            // update next sequence number
            next_seqno = packets[i]->hdr.seqno + get_data_size(packets[i]);

            // Send packet
            if(sendto(sockfd, packets[i], TCP_HDR_SIZE + get_data_size(packets[i]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("Failed to send packet\n");
            }
            
            VLOG(INFO, "Sent packet %d", packets[i]->hdr.seqno);
            //update arb_baseno
            arb_baseno = i-(window_size-1);
            // update arb_next_seqno
            arb_next_seqno = i+1;
        
            i++;
        }

    }

    return 0;
}


