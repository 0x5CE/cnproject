//cnproj.c - protocol for secured data transfer over UDP 
//			 Sends and receives the given file
//
//Usage: 	./cnproj Sender/Recveiver filename listenport
//
//Authors:	Syed Muazzam Ali Shah Kazmi
//			Muhammad Hasnain Naeem
//
//NOTE: this file contains code for both the sender and the receiver 

#include <stdio.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

enum allErrors
{
	NO_ERRORS,
	ERROR_NOT_ENOUGH_ARGS,
	ERROR_WRONG_USAGE,
	ERROR_SOCKET,
	ERROR_BIND,
	ERROR_FILE_FOPEN,
	ERROR_FILE_WRITE
};

typedef struct sockaddr_in SAin;
typedef const struct sockaddr cSA;
typedef struct sockaddr SA;

//#define SIM_LATENCY 1000		//latency in milliseconds
//#define SIM_CORRUPTION_CHECKSUM 14 //chance of error: use 0 to 14

#define SEGMENT_SIZE(s) (sizeof(s) - SEGMENT_MESSAGE_SIZE + s.length)
#define SEGMENT_MESSAGE_SIZE 1
int networkWindowSize = 5;

typedef struct Segment
{
		uint16_t checksum;
		int length;
		int seqnum;
		char message[SEGMENT_MESSAGE_SIZE];
} Segment;

int main(int argc, char *argv[]);
int fileSender(unsigned short port, char* fileName);
int fileRecveiver(unsigned short port, char* fileName);
uint16_t checksum(Segment seg);
int checkWindow(Segment *segWin, int winSize, int nextSeq, int *defectivePackets);
int saveWindow(Segment *segWin, int winSize, FILE *recvFP);
void getSegmentOrder(Segment *segWin, int winSize, int *orderArray);

//reorders and saves
int saveWindow(Segment *segWin, int winSize, FILE *recvFP)
{
		if (winSize < 1)
			return NO_ERRORS;

		int *orderArray = (int*)malloc(winSize * sizeof(int));
		getSegmentOrder(segWin, winSize, orderArray);

		int bytesWritten = 0;
		for (int i=0; i<winSize; i++)
		{
			int ordIndex = orderArray[i];	//save in that order			
			if (segWin[ordIndex].length < 1) break;	

			bytesWritten = fwrite(segWin[ordIndex].message,
				sizeof(char), segWin[ordIndex].length, recvFP);
			if (bytesWritten != segWin[ordIndex].length)
			{
				fclose(recvFP);
				return ERROR_FILE_WRITE;
			}

		}
		return NO_ERRORS;
}

//supporting function for saveWindow()
//gives packet orders in descending seqnum order
void getSegmentOrder(Segment *segWin, int winSize, int *orderArray)
{
	if (winSize < 1)
		return;

	int orderArrayN = winSize-1;
	for (int i=0; i<winSize; i++)
	{
		int largestSeq = 0;
		int largestIndex = 0;
		for (int j=0; j<winSize; j++)
		{
			if ((segWin[j].seqnum > largestSeq) && (segWin[j].seqnum > 0))
			{
				largestSeq = segWin[j].seqnum;
				largestIndex = j;
			}
		}
		segWin[largestIndex].seqnum = -1;
		orderArray[orderArrayN--] = largestIndex;
	}
}

//performs checksum, checks possible duplicates, and stores defective packets in defectivePackets array
int checkWindow(Segment *segWin, int winSize, int nextSeq, int *defectivePackets)
{
	int numDefect = 0;
    for (int i=0; i<winSize; i++)
    {
		if (segWin[i].checksum != checksum(segWin[i]))
			defectivePackets[numDefect++] = i;
    }
    return numDefect;
}

uint16_t checksum(Segment seg)
{
    uint16_t *segBytes = (uint16_t*) &seg;
    int n = seg.length;
	
	if (n <= sizeof(seg.checksum))
		return 0;

    uint16_t csum = 0xffff; 

    for (int i=sizeof(seg.checksum); i < n/2; i++)
    {
        csum += ntohs(segBytes[i]);
        if (csum > 0xffff)
            csum -= 0xffff;
    }
    //last remaining bytes (if odd length)
    if (n & 1)
    { 
        csum += ntohs(segBytes[n/2]);
        if (csum > 0xffff)
            csum -= 0xffff;
    }
    return csum;
}

int fileSender(unsigned short port, char* fileName)
{
	printf("File sender started with:\n\tfileName: \t%s\n\
		\tport: \t\t%d\n", fileName, port);

	//sender (server), receiver (client) details (IP, ports, etc.)
	SAin servaddr, cliaddr;

	Segment *segWin = (Segment*) malloc(sizeof(Segment) * networkWindowSize);
	Segment segRecv;

	memset(&servaddr, 0, sizeof(servaddr));
	memset(&cliaddr, 0, sizeof(cliaddr));

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0)
		return ERROR_SOCKET;

	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);
	servaddr.sin_addr.s_addr = INADDR_ANY;
	
	if (bind(sockfd, (cSA*)&servaddr, sizeof(servaddr)) < 0)
	{
		close(sockfd);
		return ERROR_BIND;
	}

	int len, n, byteRead;
	len = sizeof(cliaddr);

	n = recvfrom(sockfd, (Segment *)&segRecv, sizeof(segRecv), MSG_WAITALL,
		(SA*) &cliaddr, &len);
	printf("Connection established\n");

	FILE *sendFP = fopen(fileName, "r");
	if (!sendFP)
		return ERROR_FILE_FOPEN;

	int pktCount = 0, glbSeqnum = 0;
	segWin[pktCount].seqnum = 0;
	len = sizeof(cliaddr);

	while ((byteRead = fread(segWin[pktCount].message, sizeof(char),
		 SEGMENT_MESSAGE_SIZE, sendFP)))
	{
		segWin[pktCount].length = byteRead;
		segWin[pktCount].seqnum = glbSeqnum++;

		segWin[pktCount].checksum = checksum(segWin[pktCount]);

		n = sendto(sockfd, (Segment *) &segWin[pktCount],
			sizeof(segWin[pktCount]), MSG_CONFIRM, (cSA*) &cliaddr, len);

		//wait for ack
		if (pktCount == networkWindowSize-1)
		{
			do
			{
				n = recvfrom(sockfd, (Segment *)&segRecv, sizeof(segRecv),
				 MSG_WAITALL, (SA*) &cliaddr, &len);

				if (segRecv.seqnum >= 0 && segRecv.seqnum < networkWindowSize)
				{
					//resend requested packet
					n = sendto(sockfd, (Segment *) &segWin[segRecv.seqnum],
						sizeof(segWin[segRecv.seqnum]), MSG_CONFIRM, (cSA*) &cliaddr, len);

					printf("Selective repeat requested.\n");
				}
			}
			while (segRecv.seqnum != -1);	//-1 ack = Ok to proceed

			printf("RECEIVING ACK.........%d\n", pktCount);
			pktCount = -1;
		}
		pktCount++;
	}

	//eof
	segWin[0].length = 0;
	segWin[0].seqnum = glbSeqnum;
	segWin[0].checksum = checksum(segWin[0]);
	n = sendto(sockfd, (Segment *) &segWin[0], sizeof(segWin[0]), MSG_CONFIRM,
			(cSA*) &cliaddr, len); 

	fclose(sendFP);
	close(sockfd);
	return 0;
}

int fileRecveiver(unsigned short port, char* fileName)
{
	printf("File receiver started with:\n\tfileName: \t%s\n\
		\tport: \t\t%d\n", fileName, port);

	SAin servaddr;
	memset(&servaddr, 0, sizeof(servaddr));

	Segment *segWin = (Segment*) malloc(sizeof(Segment) * networkWindowSize);
	Segment seg;
	seg.seqnum = 0;
	seg.length = -1;

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0)
		return ERROR_SOCKET;

	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);
	servaddr.sin_addr.s_addr = INADDR_ANY;

	int n, len, bytesWritten;

	sendto(sockfd, (Segment *)&seg, sizeof(seg), MSG_CONFIRM,
		(cSA*) &servaddr, sizeof(servaddr));
	printf("Handshake message sent.\n");

	FILE *recvFP = fopen(fileName, "w");
	if (!recvFP)
	{
		close(sockfd);
		return ERROR_FILE_FOPEN;
	}

	len = sizeof(servaddr);
	int pktCount = 0, glbSeqnum = 0;
	int *defectivePackets = (int*)malloc(sizeof(int)*networkWindowSize);

	while ((n = recvfrom(sockfd, (Segment *)&segWin[pktCount],
		sizeof(segWin[pktCount]), MSG_WAITALL, (SA*) &servaddr, &len)))
	{
		int isEOF = (segWin[pktCount].length == 0);

//Simulate latency
#ifdef SIM_LATENCY
		usleep(1000);
#endif
//Simulate corrupted papcket (with checksum)
#ifdef SIM_CORRUPTION_CHECKSUM
		if (rand() % 100 < SIM_CORRUPTION_CHECKSUM) segWin[pktCount].checksum = 1234;
#endif
		//send ack
		if (pktCount == networkWindowSize - 1
		 || isEOF)
		{
			int nDef = 0;
			while ( ( nDef = checkWindow(segWin, pktCount+1, seg.seqnum, defectivePackets)))
			{
				//get correct packets for corrupted/duplicate packets
				for (int i=0; i<nDef; i++)
				{
						seg.seqnum = defectivePackets[i];
						sendto(sockfd, (Segment *)&seg, sizeof(seg), MSG_CONFIRM,
							(cSA*) &servaddr, sizeof(servaddr));

						printf("Recovering packet: %d\n", seg.seqnum);

						n = recvfrom(sockfd, (Segment *)&segWin[defectivePackets[i]],
							sizeof(Segment), MSG_WAITALL, (SA*) &servaddr, &len);
						glbSeqnum++;
				}
			}

			//all okay, now save
			if (saveWindow(segWin, pktCount+1, recvFP) != NO_ERRORS)
			{
				close(sockfd);
				return ERROR_FILE_WRITE;
			}

			seg.seqnum = -1;	//all ok, proceed
			sendto(sockfd, (Segment *)&seg, sizeof(seg), MSG_CONFIRM,
				(cSA*) &servaddr, sizeof(servaddr));
			printf("SENDING ACK.........%d\n", pktCount);
			pktCount = -1;
		}
		
		if (isEOF)
		{
			printf("EOF reached.........Packets transmitted: %d\n", glbSeqnum);
			break;	
		}

		pktCount++;
		glbSeqnum++;
		len = sizeof(servaddr);

		//segWin[pktCount].message[segWin[pktCount].length] = '\0';
		//printf("Client: %d %d %d %s\n", segWin[pktCount].checksum, 
		//segWin[pktCount].length, segWin[pktCount].seqnum, segWin[pktCount].message);
	}
		
	fclose(recvFP);
	close(sockfd);
	return 0;
}

int main(int argc, char *argv[])
{
	srand(time(0));
	int status = ERROR_NOT_ENOUGH_ARGS; 
	do
	{
		if (argc != 4) break;

		unsigned short port = atoi(argv[3]);
		signal(SIGPIPE, SIG_IGN);
		status = ERROR_WRONG_USAGE;	
		
		if (strcmp(argv[1], "Sender") == 0)
		{
			status = fileSender(port, argv[2]);
		}
		else if (strcmp(argv[1], "Receiver") == 0)
		{
			status = fileRecveiver(port, argv[2]);
		}
		else break;	
		
	} while (false);

	//print errors
	switch (status)
	{
		case 0:
			break; //no error
		case ERROR_NOT_ENOUGH_ARGS:
		case ERROR_WRONG_USAGE:
			printf("Usage: ./cnproj Sender/Recveiver filename listenport\n");
			return 1;
		case ERROR_SOCKET:
			perror("Socket"); return 1;
		case ERROR_BIND:
			perror("Bind"); return 1;
		case ERROR_FILE_FOPEN:
			perror("fopen"); return 1;
		case ERROR_FILE_WRITE:
			perror("fwrite"); return 1;
		default:
			printf("Unknown error.\n"); return 1; 
	}
	return 0;
}
