#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip_icmp.h>
#include "sockwrap.h"
#include "icmp.h"
#include "ip_list.h"

#define TIMEOUT 1000  /* Time of waiting for packets with last set TTL in miliseconds (= 1 second) */
#define TTL_LIMIT 30
#define REQUESTS_PER_TTL 3
#define BUFFER_SIZE 128

// Returns the difference between two times in miliseconds.
double timeDifference(struct timeval start, struct timeval end) {
    return (end.tv_sec - start.tv_sec)*1000.0 + (end.tv_usec - start.tv_usec)/1000.0;
}

int main(int argc, char* argv[]) {
    
    if(argc != 3) { printf("Usage: ./prog hop-distance <IP_address>\n"); exit(1); }

	int hops = (int) strtol(argv[1], NULL, 10);

	if (hops > TTL_LIMIT) { printf("hop-distance can't be greater than 30\n"); exit(1);  }
    
    struct sockaddr_in remoteAddr;
    bzero(&remoteAddr, sizeof(remoteAddr));
    remoteAddr.sin_family = AF_INET;
    Inet_pton(AF_INET, argv[1], &remoteAddr.sin_addr);  // 如果給的ip address是錯的，則會產生error message
              
    int pid = getpid();  // get process id for later identification
    
    int sockId = Socket(AF_INET, SOCK_RAW, IPPROTO_ICMP); 
    struct timeval begin, current;
    begin.tv_sec = 0;
    begin.tv_usec = 1000;  // 1 ms
    Setsockopt(sockId, SOL_SOCKET, SO_RCVTIMEO, &begin, sizeof(begin));  // 設定等待封包的時間
    
    char icmpRequestBuffer[BUFFER_SIZE], replyBuffer[BUFFER_SIZE];  // ICMP request 和 收到的IP封包

	// 建立ICMP echo request
    struct icmp *icmpRequest = (struct icmp *) icmpRequestBuffer;
    icmpRequest->icmp_type = ICMP_ECHO;
    icmpRequest->icmp_code = htons(0);  // htons(x) returns the value of x in TCP/IP network byte order
    icmpRequest->icmp_id = htons(pid);    
    
    int ttl, sequence = 0, repliedPacketsCnt, i;
    bool stop = 0;  // set to true, when echo reply has been received
    double elapsedTime;  // variable used to compute the average time of responses
    struct timeval sendTime[REQUESTS_PER_TTL];  // send time of a specific packet
    ip_list *ipsThatReplied;  // list of IPs that replied to echo request
    
    for(ttl=1; ttl<=hops; ttl++) {
		repliedPacketsCnt = 0;
		elapsedTime = 0.0;
		ipsThatReplied = createIpList();

		for(i=1; i<=REQUESTS_PER_TTL; i++) {
			Setsockopt(sockId, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));  // set TTL of IP packet that is being sent
			icmpRequest->icmp_seq = htons(++sequence);  // set sequence number, for later identification
			
			icmpRequest->icmp_cksum = 0;
			icmpRequest->icmp_cksum = in_cksum((uint16_t*) icmpRequest, ICMP_HEADER_LEN, 0);
			
			gettimeofday(&sendTime[(sequence-1) % REQUESTS_PER_TTL], NULL);
			Sendto(sockId, icmpRequestBuffer, ICMP_HEADER_LEN, 0, &remoteAddr, sizeof(remoteAddr));
		}
	
		gettimeofday(&begin, NULL);  // get time after sending the packets
	
		while(repliedPacketsCnt < REQUESTS_PER_TTL) {
		  
			int RecvRetVal = Recvfrom(sockId, replyBuffer, BUFFER_SIZE, 0, 0, 0);  // wait 1 ms for a packet (at most)
			gettimeofday(&current, NULL);
			
			if(RecvRetVal < 0) {
				if(timeDifference(begin, current) > TIMEOUT) break;
				continue;
			}
			
			struct ip *reply = (struct ip *) replyBuffer;
			
			if(reply->ip_p != IPPROTO_ICMP) continue;  // 確認封包的protocol是否為ICMP
			
			struct icmp *icmpHeader = (struct icmp *) (replyBuffer + reply->ip_hl*4);  // 從封包取出ICMP header
			
			// 檢查ICMP的type
			if(icmpHeader->icmp_type != ICMP_ECHOREPLY && 
			  !(icmpHeader->icmp_type == ICMP_TIME_EXCEEDED && 
			  icmpHeader->icmp_code == ICMP_EXC_TTL)) continue;
			
			// 若ICMP的type為time_exceeded，shift icmpHeader to the copy of our request
			if(icmpHeader->icmp_type == ICMP_TIME_EXCEEDED)
			icmpHeader = (struct icmp *) (icmpHeader->icmp_data + ((struct ip *) (icmpHeader->icmp_data))->ip_hl*4);
			
			// is icmp_id equal to our pid and it's one of the latest (three) packets sent?
			if(ntohs(icmpHeader->icmp_id) != pid || sequence - ntohs(icmpHeader->icmp_seq) >= REQUESTS_PER_TTL) continue;
			
			elapsedTime += timeDifference(sendTime[(ntohs(icmpHeader->icmp_seq)-1) % REQUESTS_PER_TTL], current);
			insert(ipsThatReplied, reply->ip_src);
			repliedPacketsCnt++;
			
			if(icmpHeader->icmp_type == ICMP_ECHOREPLY) stop = 1;
		}
	
		// Prints router info
		printf("%2d. ", ttl);
		if(repliedPacketsCnt == 0) { printf("*\n"); continue; }
		printIpList(ipsThatReplied);
		destroyIpList(ipsThatReplied);
			
		if(repliedPacketsCnt == REQUESTS_PER_TTL) printf("%.1f ms\n", elapsedTime / repliedPacketsCnt);
		else printf("\t???\n");
	
		if(stop == 1) break;
    }
    return 0;
}
