#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"


/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received,expected seq_no, sequence number");

    clientlen = sizeof(clientaddr);
    int next_seqno = 0;

    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }

        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        /* 
         * sendto: ACK back to the client 
         */
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, next_seqno, recvpkt->hdr.seqno);

        // if last packet, send ACK and break out of loop
        if (recvpkt->hdr.data_size == 0 && next_seqno == recvpkt->hdr.seqno)
        {
            VLOG(INFO, "End Of File has been reached");

            // send the last ACK 8 times(a hack to circumvent the loss of the last ACK)
            // this is a hack to make sure the client has received the last packet and is not waiting for more
            // since we are breaking out of the loop
            for (int i = 0; i < 8;i++){ 
                sndpkt = make_packet(0);
                sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
                sndpkt->hdr.ctr_flags = ACK;
                if(sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)&clientaddr, clientlen) < 0) {
                    error("Could not send ACK\n");
                }
            } 	
            // close the file
            fclose(fp);

            // break out of the loop
            break;
        }

        // if the packet is the next packet in the sequence, send an ACK for it and write to file
        else if(next_seqno == recvpkt->hdr.seqno) {
            VLOG(INFO, "Packet in order");
            next_seqno += recvpkt->hdr.data_size;
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
            sndpkt->hdr.ctr_flags = ACK;
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)&clientaddr, clientlen) < 0) {
                error("Could not send ACK\n");
            }
        }

        // if the packet is not the next packet in sequence, send the last ACK again
        else if(next_seqno != recvpkt->hdr.seqno) {
            VLOG(INFO, "Packet out of order");
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = next_seqno;
            sndpkt->hdr.ctr_flags = ACK;
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)&clientaddr, clientlen) < 0) {
                error("Could not send ACK\n");
            }
        }

    }

    close(sockfd); // close the socket

    return 0;
}
