/*
 ============================================================================
 Name        : testTCPdrv.c
 Author      : stj
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include <pthread.h>
#include <semaphore.h>
#include <errno.h>

// sockets
#ifdef WIN32
	#ifndef WINVER
		// set min win version to Win XP
		#define WINVER 0x0501
	#endif
	//use lib: ws2_32
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else

	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <netdb.h>

	#include <sys/un.h>
	#include <unistd.h>
	#include <arpa/inet.h>

	#define ADDR_ANY	INADDR_ANY
	#define SOCKET_ERROR	(-1)
	#define INVALID_SOCKET (SOCKET)(~0)
	#define closesocket(x) (close(x))

	typedef int	SOCKET;
	typedef struct sockaddr_in SOCKADDR_IN;
	typedef struct sockaddr SOCKADDR;

#endif


typedef int (* TfkpTCPcallback) (uint8_t * pData, size_t amount);

// size of the header
#define dStjTCPSocketControlMsg (sizeof(uint_32))

// start data msg struct
// <uint_32> id = 's'
// <uint_32> len

// res struct
// <uint_32> id = 'r'
// <uint_32> error code (0 = no error)

enum eStjTCPSocketControlMsgIDs {
	eStjTCPSocketControlMsgID_start = 's',
	eStjTCPSocketControlMsgID_packet = 'p',
	eStjTCPSocketControlMsgID_result = 'r'
};

enum eStjTCPSocketControlMsgErrorIDs {
	eStjTCPSocketControlMsgErrorID_noError = 0,
	eStjTCPSocketControlMsgErrorID_otherError,
	eStjTCPSocketControlMsgErrorID_socket,
	eStjTCPSocketControlMsgErrorID_msgID,
	eStjTCPSocketControlMsgErrorID_realloc,
	eStjTCPSocketControlMsgErrorID_amount,
	eStjTCPSocketControlMsgErrorID_wrongPacket,

};

//! type to control a udp socket based message communication
typedef struct SstjTCPSocketControl {
	pthread_t			srvThr;

	SOCKET 				sCli;	//!< socket for the input
	SOCKET 				sSrv;	//!< socket for the output

	struct sockaddr_in 	sAddrCli; //!< client address
	int					cliConnectedFlag; //!< <>0 if the client is connected

	uint8_t *			pMsgBuffer;
	size_t				msgBufferSize;

	sem_t 				serverSign;
	TfkpTCPcallback		rxCB;

	int					maxTXsize;
} TstjTCPSocketControl;

//! a global variable to control a udp message based communication
TstjTCPSocketControl gTCPsocketControl = {
	.srvThr = NULL,
	.sCli = -1,
	.sSrv = -1,
	.cliConnectedFlag = 0,
	.pMsgBuffer = NULL,
	.msgBufferSize = 0,
};

static inline int _TCPcontrolRecvResult(SOCKET s) {
	int r;
	uint32_t contrlMsg[2];

	// recv that the server is ready to transmit
	r = recv(s , (char *)contrlMsg , sizeof(contrlMsg) , 0);
	if(r < 0) {
		return eStjTCPSocketControlMsgErrorID_socket;
	}
	if (r != sizeof(contrlMsg)) {
		return eStjTCPSocketControlMsgErrorID_amount;
	}
	if (contrlMsg[0] != eStjTCPSocketControlMsgID_result) {
		return eStjTCPSocketControlMsgErrorID_msgID;
	}

	return contrlMsg[1];
}

static inline int _TCPcontrolSendResult(SOCKET s, uint32_t errorCode) {
	uint32_t contrlMsg[2];
	int r;

	contrlMsg[0] = eStjTCPSocketControlMsgID_result;
	contrlMsg[1] = errorCode;
	r = send(s , (char *)contrlMsg , sizeof(contrlMsg) , 0);
	if (r < 0) return eStjTCPSocketControlMsgErrorID_socket;
	return eStjTCPSocketControlMsgErrorID_noError;
}

//! sends a block of data
int TCPcontrolSend(uint8_t * pD, size_t dataSize) {
	int r;
	uint32_t contrlMsg[2];
	uint32_t p;
	uint32_t packets;
	uint8_t * pB;
	size_t	am, amTotal;

	// check if we have to connect
	if (!gTCPsocketControl.cliConnectedFlag) {
		if (connect(gTCPsocketControl.sCli , (struct sockaddr *)&gTCPsocketControl.sAddrCli , sizeof(gTCPsocketControl.sAddrCli)) < 0){
			gTCPsocketControl.cliConnectedFlag = 0;
			return -1;
		} else {
			gTCPsocketControl.cliConnectedFlag = 1;
		}

	}
	//  ok we are connected - lets send the data
	start:

	contrlMsg[0] = eStjTCPSocketControlMsgID_start;
	contrlMsg[1] = dataSize;
	// send that we what to transmit some data
	r = send(gTCPsocketControl.sCli , (char *)contrlMsg , sizeof(contrlMsg) , 0);
	if(r < 0) {
		return -2;
	}
	// recv that the server is ready to transmit
	r = _TCPcontrolRecvResult(gTCPsocketControl.sCli);
	if (eStjTCPSocketControlMsgErrorID_socket == r) return -3;
	if (eStjTCPSocketControlMsgErrorID_amount == r) goto start;

	// ok let's send
	packets = dataSize / gTCPsocketControl.maxTXsize;
	if (dataSize % gTCPsocketControl.maxTXsize) packets++;
	pB = pD;
	amTotal = dataSize;

	for (p = 0; p < packets; p++) {
		// send packet pre header
		contrlMsg[0] = eStjTCPSocketControlMsgID_packet;
		contrlMsg[1] = p;
		r = send(gTCPsocketControl.sCli , (char *)contrlMsg , sizeof(contrlMsg) , 0);
		if(r < 0) {
			return -4;
		}
		r = _TCPcontrolRecvResult(gTCPsocketControl.sCli);
		if (eStjTCPSocketControlMsgErrorID_socket == r) return -5;
		if (eStjTCPSocketControlMsgErrorID_amount == r) goto start;

		am = (amTotal > gTCPsocketControl.maxTXsize) ? gTCPsocketControl.maxTXsize : amTotal;

sendPacket:
		r = send(gTCPsocketControl.sCli ,(char *) pB ,am , 0);
		if(r < 0) {
			return -5;
		}

		// get ack from the server
		r = _TCPcontrolRecvResult(gTCPsocketControl.sCli);
		if (eStjTCPSocketControlMsgErrorID_socket == r) return -3;
		if (eStjTCPSocketControlMsgErrorID_amount == r) goto sendPacket;

		pB += am;
		amTotal -= am;
	}
	return r;
}


//! the message pump
void * TCPcontrolMsgPump (void *pParams) {
	int 				r;
	uint32_t 			contrlMsg[2];
	struct sockaddr_in 	cliAddr;
	SOCKET 				sCli;
	uint32_t			dataSize;
	socklen_t			cliAddrSize;
	uint32_t 			packets;
	uint8_t * 			pB;
	size_t				am, amTotal;
	uint32_t			p;


	sem_post(&gTCPsocketControl.serverSign);

	//accept connection from an incoming client
	cliAddrSize = sizeof(struct sockaddr_in);
	sCli = accept(gTCPsocketControl.sSrv, (struct sockaddr *)&cliAddr, (socklen_t*)&cliAddrSize);
	if (sCli < 0) goto end;

	// run the pump
	for (;;) {
		// ok we are connected
		// read start message
		r = recv(sCli , (char *)contrlMsg , sizeof(contrlMsg), 0);
		if (r < 0) goto end;
		if (r != sizeof(contrlMsg)) {
			_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_amount);
			continue;
		}
		if (contrlMsg[0] != eStjTCPSocketControlMsgID_start) {
			_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_msgID);
			continue;
		}

		dataSize = contrlMsg[1];
		// check if we have to realloc the rx buffer
		if (gTCPsocketControl.msgBufferSize < dataSize) {
			 uint8_t *pNB = realloc(gTCPsocketControl.pMsgBuffer, dataSize);
			 if (!pNB) {
				 _TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_realloc);
				 continue;
			 }
			 gTCPsocketControl.pMsgBuffer = pNB;
			 gTCPsocketControl.msgBufferSize = dataSize;
		}

		_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_noError);

		// recv data
		packets = dataSize / gTCPsocketControl.maxTXsize;
		if (dataSize % gTCPsocketControl.maxTXsize) packets++;
		pB = gTCPsocketControl.pMsgBuffer;
		amTotal = dataSize;

		for (p = 0; p < packets; p++) {
			// receive packet header
			r = recv(sCli , (char *)contrlMsg , sizeof(contrlMsg), 0);
			if (r < 0) goto end;
			if (r != sizeof(contrlMsg)) {
				_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_amount);
				continue;
			}
			if (contrlMsg[0] != eStjTCPSocketControlMsgID_packet) {
				_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_msgID);
				continue;
			}
			if (contrlMsg[1] != p) {
				_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_wrongPacket);
				continue;
			}
			_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_noError);

			am = (amTotal > gTCPsocketControl.maxTXsize) ? gTCPsocketControl.maxTXsize : amTotal;

			// ok the next message will contain the data
recvPacket:
			r = recv(sCli , (char *)pB , am, 0);
			if (r < 0) goto end;
			if (r != am) {
				_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_amount);
				goto recvPacket;
			}

			_TCPcontrolSendResult(sCli, eStjTCPSocketControlMsgErrorID_noError);
			pB += am;
			amTotal -= am;
		}

		// handle message
		gTCPsocketControl.rxCB(gTCPsocketControl.pMsgBuffer , dataSize);
		continue;
	}
end:
	sem_post(&gTCPsocketControl.serverSign);
	return (void *) -1;
}

//! init
int TCPcontrolInit (
		int 			serverPort,		//!< server tx port number - best over 1000
		const char * 	szClient,		//!< "family-PC" or "192.168.1.3"
		int				clientPort,		//!< client tx port number
		TfkpTCPcallback rxCB,			//!< the rx data callback
		size_t			rxBufferSize,	//!< the size of the rx buffer
		size_t			maxTCPdataSize 	//!< maximum size of a TCP datagram (400 Bytes seems a good size)
	) {
#ifdef WIN32
	// local data
	WSADATA		wsaData;

	// start sockets
    if ((WSAStartup(MAKEWORD(2, 2), &wsaData))) {
    	perror("WSAStartup failed!");
        return -1;
    }
#endif
    char *				szIPserver;
    char *				szIPclient;
    struct hostent *	pHostDescr;
    struct sockaddr_in 	sAddr;

    // -----------------
    // get ip strings

    // get ip of the server
	pHostDescr = gethostbyname("localhost");
	// check if found a host
	if (!pHostDescr) {
		return -11;
	}
	szIPserver = inet_ntoa(*(struct in_addr*)*pHostDescr->h_addr_list);

    // get ip of the client
    if (strcmp(szClient, "")) {
    	pHostDescr = gethostbyname(szClient);
    } else {
    	pHostDescr = gethostbyname("localhost");
    }
    // check if found a host
	if (!pHostDescr) {
		return -12;
	}
	szIPclient = inet_ntoa(*(struct in_addr*)*pHostDescr->h_addr_list);


    // -----------------
    // try to create sockets

	// try to create socket for the server
	gTCPsocketControl.sSrv = socket(PF_INET , SOCK_STREAM, IPPROTO_TCP);
	if (-1 == gTCPsocketControl.sSrv) return -21;
	// try to create socket for the client
	gTCPsocketControl.sCli = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (-1 == gTCPsocketControl.sCli) return -22;

    // -----------------
    // bind input to IP and port
	memset(&sAddr,0,sizeof(sAddr));
	sAddr.sin_family = PF_INET;
	sAddr.sin_addr.s_addr = INADDR_ANY;
	sAddr.sin_port = htons( serverPort );

	// bind server socket to address
	if (bind(gTCPsocketControl.sSrv, (SOCKADDR *)&sAddr, sizeof(SOCKADDR_IN))) {
		return -31;
    }
	// and listen for incoming connections
	if (listen(gTCPsocketControl.sSrv , 3)) {
		return -32;
	}

	// -----------------
	// connect output to IP and port
	memset(&gTCPsocketControl.sAddrCli,0,sizeof(sAddr));
	gTCPsocketControl.sAddrCli.sin_family = PF_INET;
	gTCPsocketControl.sAddrCli.sin_addr.s_addr = inet_addr(szIPclient);
	gTCPsocketControl.sAddrCli.sin_port = htons( clientPort );

	if (connect(gTCPsocketControl.sCli , (struct sockaddr *)&gTCPsocketControl.sAddrCli , sizeof(gTCPsocketControl.sAddrCli)) < 0){
		gTCPsocketControl.cliConnectedFlag = 0;
	} else {
		gTCPsocketControl.cliConnectedFlag = 1;
	}


	// create sign semaphore
	sem_init(&gTCPsocketControl.serverSign, 0, 0);

	// create buffers
	gTCPsocketControl.pMsgBuffer = malloc(rxBufferSize);
	if (!gTCPsocketControl.pMsgBuffer) {
		return -32;
	}
	gTCPsocketControl.msgBufferSize = rxBufferSize;

	// set callback
	gTCPsocketControl.rxCB = rxCB;

	gTCPsocketControl.maxTXsize = maxTCPdataSize;

	// start rx thread
	if(pthread_create(&gTCPsocketControl.srvThr , NULL, TCPcontrolMsgPump, NULL)) {
		return -40;
	}
	// wait till rx server is running
	sem_wait(&gTCPsocketControl.serverSign);

	return 0;
}

//! closes the TCP server and client
void TCPcontrolClose () {
	closesocket (gTCPsocketControl.sSrv);
	closesocket (gTCPsocketControl.sCli);

	free(gTCPsocketControl.pMsgBuffer);

	memset(&gTCPsocketControl, 0, sizeof(TstjTCPSocketControl));

#ifdef WIN32
	WSACleanup();
#endif
}


//! inits the TCP control via stdin inputs
int TCPcontrolInitFromStdIn (
		TfkpTCPcallback rxCB,			//!< the rx data callback
		size_t			rxBufferSize,	//!< the size of the rx buffer
		size_t			maxTCPdataSize 	//!< maximum size of a TCP datagram (400 Bytes seems a good size)
	) {
	int srvPort;
	int clientPort;
	const size_t ipLen = 256;
	char szIP[ipLen];
	const size_t dummyStrLen = 100;
	char szDummy[dummyStrLen];

	int r;

	printf("====| TCP client/server setup |====\n");
	printf("server listen port: ");
	fgets(szDummy, dummyStrLen, stdin);
	szDummy[strcspn(szDummy, "\r\n")] = 0;
	srvPort = atoi(szDummy);

	printf("client send IP address or name: ");
	fgets(szIP, 255, stdin);
	szIP[strcspn(szIP, "\r\n")] = 0;

	printf("client port: ");
	fgets(szDummy, dummyStrLen, stdin);
	szDummy[strcspn(szDummy, "\r\n")] = 0;
	clientPort = atoi(szDummy);

	r = TCPcontrolInit (
			srvPort,		//!< server port number - best over 1000
			szIP,			//!< "family-PC" or "192.168.1.3"
			clientPort,		//!< client port number
			rxCB,			//!< the rx data callback
			rxBufferSize,	//!< the size of the rx buffer
			maxTCPdataSize 	//!< maximum size of a TCP datagram (400 Bytes seems a good size)
		);

	if (!r) {
		printf("setup finished successfully!\n");
		printf("===================================\n");
	} else {
		printf("setup error: %i \n", r);
		printf("===================================\n");
	}
	return r;
}

// -----------------------------------------
// test

enum eStates {
	eState_std = 0,
	eState_stressTest = 1,
	eState_multiTX = 2
};

int stateID = eState_std;
#define  dSTsize (1024 * 1024)
uint8_t STB[dSTsize];


int rxCB (uint8_t * pData, size_t amount) {
	size_t i;

	switch (stateID) {
		case eState_std:
			pData[amount] = 0;
			printf("rx: %s\n",pData);
			break;
		case eState_stressTest:
			for (i = 0; i < dSTsize; i++) {
				if (pData[i] != (uint8_t)((size_t)i & 0xFF)) {
					fprintf(stderr, "stress test error at position %i\n",(int) i);
					fflush(stdout);
					return 0;
				}
			}
			printf("rx: stress test successful\n");
			break;
		case eState_multiTX:
			printf("rx %iBytes\n", (int)amount);
			break;
	}
	fflush(stdout);
	return 0;
}


int main(void) {
	const size_t dummyStrLen = 1024;
	char szDummy[dummyStrLen];
	size_t i;
	int r, am, j;

	// pre init for the stress test
	for (i = 0; i < dSTsize; i++) {
		STB[i] = (uint8_t)((size_t)i & 0xFF);
	}


	printf("TCP demo\n");

	if (TCPcontrolInitFromStdIn(rxCB, 4096, 500)) goto errorExit;


	printf("commands:\n s - send\n t - tx stress test\n a - activate/deactivate rx for stress test\n m - multi tx test\n h - help\n e - exit\n");
	for(;;) {
		printf("command: ");
		fgets(szDummy, dummyStrLen, stdin);
		switch(tolower(szDummy[0])) {
			case 's':
				stateID = eState_std;
				fgets(szDummy, dummyStrLen, stdin);
				szDummy[strcspn(szDummy, "\r\n")] = 0;
				r = TCPcontrolSend((uint8_t *)szDummy, strlen(szDummy)+1);
				if(r) {
					fprintf(stderr,"sending data failed with code %i(%s)\n", r, strerror(errno));
				} else {
					printf("succeeded\n");
				}
				break;
			case 't':
				printf("sending packets...\n");
				r = TCPcontrolSend(STB, dSTsize);

				if (r) {
					fprintf(stderr,"stress test sending data failed with code %i\n", r);
				} else {
					printf("succeeded\n");
				}
				break;
			case 'a':
				stateID = eState_stressTest;
				printf("stress test RX now active\n");
				break;
			case 'm':
				stateID = eState_multiTX;
				printf("amount of transmissions: ");
				fgets(szDummy, dummyStrLen, stdin);
				szDummy[strcspn(szDummy, "\r\n")] = 0;
				am = atoi(szDummy);
				for (j = 0; j < am; j++) {
					printf("tm %i...", j);
					sprintf(szDummy,"tm %i",j);
					r = TCPcontrolSend((uint8_t *)szDummy, strlen(szDummy)+1);
					if (!r) printf("successful\n");
						else printf("failed\n");
				}
				break;
			case 'h':
				printf("commands:\n s - send\n t - tx stress test\n a - activate/deactivate rx for stress test\n m - multi tx test\n h - help\n e - exit\n");
				break;
			case 'e':
				goto stdExit;
		}
	}


stdExit:

	TCPcontrolClose ();
	return EXIT_SUCCESS;

errorExit:

	TCPcontrolClose ();
	return EXIT_FAILURE;
}
