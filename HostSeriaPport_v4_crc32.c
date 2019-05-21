/*
Author: Roger Philp
Date: 25/03/19
This program is one aof a pair of programs to transfer files between an Arduino Mega SD card
and a host linux system or bash shell under windows. 
Transfer is bidirectional: to the host and from the host, 
with a very simplistic command line interface.

The complimentary program that needs to be loaded on to the Arduino is:

rnpSerial_v4_crc32



The program connects to:
 	char *portname = "/dev/ttyS5";
	which is comm port 5
	baudrate 115200, 8 bits, no parity, 1 stop bit
	
Transfers occur with 32bit crc checking

*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>

#define  uint32_t u_int32_t
#define  uint16_t u_int16_t

unsigned char EOT = 0x01;
unsigned char BOT = 0x02;
unsigned char LOK = 0x03;
unsigned char SYNC = 0x04;
unsigned char OK = 0x05;
unsigned char RSD = 0x06;
unsigned char SOK = 0x07;
unsigned char NOK = 0x08;

#define RETRYCOUNT 2

//fallocate -l $((20*1024)) file.txt

//	int bufSize = 1024;
const int bufSize = 64;
int inputBufferSize = 64;
unsigned char oneKbuf[64];

int32_t crcX = 0xffffffff;
int32_t poly = 0x11223344;
int32_t initX = 0x55667788;

unsigned char tmpCrc = 0;
int crcSize = 4;

typedef struct header {
	int32_t fileSize;
	int32_t bufSize;
	//	int32_t numFrames;
	int32_t crcX;
	unsigned char fileName[64];
	int32_t poly;
	int32_t initX;
	uint32_t crcCheck;
} header;

union crcOverlap {
	uint32_t crcInt;
	unsigned char crcArray[4];
};

union crcOverlap crcClcData;
union crcOverlap crcSntData;
union crcOverlap crcRcvData;

void cleanUp(int fd){
	unsigned char ch;
	while(1) {
		//	printf("here \n");
		read(fd, &ch, 1);
		if (ch == EOT) break;
		printf("%c", ch);
	}
}

int set_interface_attribs(int fd, int speed)
{
	struct termios tty;

	if (tcgetattr(fd, &tty) < 0) {
		printf("Error from tcgetattr: %s\n", strerror(errno));
		return -1;
	}

	cfsetospeed(&tty, (speed_t)speed);
	cfsetispeed(&tty, (speed_t)speed);

	tty.c_cflag |= (CLOCAL | CREAD);    /* ignore modem controls */
	tty.c_cflag &= ~CSIZE;
	tty.c_cflag |= CS8;         /* 8-bit characters */
	tty.c_cflag &= ~PARENB;     /* no parity bit */
	tty.c_cflag &= ~CSTOPB;     /* only need 1 stop bit */
	tty.c_cflag &= ~CRTSCTS;    /* no hardware flowcontrol */

	/* setup for non-canonical mode */
	tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tty.c_oflag &= ~OPOST;

	/* fetch bytes as they become available */
	tty.c_cc[VMIN] = 1;
	tty.c_cc[VTIME] = 1;

	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
		printf("Error from tcsetattr: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

void set_mincount(int fd, int mcount)
{
	struct termios tty;

	if (tcgetattr(fd, &tty) < 0) {
		printf("Error tcgetattr: %s\n", strerror(errno));
		return;
	}

	tty.c_cc[VMIN] = mcount ? 1 : 0;
	tty.c_cc[VTIME] = 5;        /* half second timer */

	if (tcsetattr(fd, TCSANOW, &tty) < 0)
		printf("Error tcsetattr: %s\n", strerror(errno));
}

void
set_blocking (int fd, int should_block)
{
	struct termios tty;
	memset (&tty, 0, sizeof tty);
	if (tcgetattr (fd, &tty) != 0)
	{
		printf ("error %d from tggetattr", errno);
		return;
	}

	tty.c_cc[VMIN]  = should_block ? 1 : 0;
	tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

	if (tcsetattr (fd, TCSANOW, &tty) != 0)
		printf("error %d setting term attributes", errno);
}

unsigned char *removeSpace(char *text, int len){
	unsigned char *cmdStr;

	cmdStr  = (unsigned char *)malloc(len);

	int c, d;
	while(text[0] == ' '){
		for(c = 0; c < strlen(text); c++) text[c] = text[c + 1];
	}

	c = 0, d = 0;
	while (text[c] != '\0') {
		if (text[c] == ' ') {
			int temp = c + 1;
			if (text[temp] != '\0') {
				while (text[temp] == ' ' && text[temp] != '\0') {
					if (text[temp] == ' ') {
						c++;
					}
					temp++;
				}
			}
		}
		cmdStr[d] = text[c];
		c++;
		d++;
	}

	cmdStr[d] = '\0';
	return cmdStr;
}

// add in the crc material
const int32_t order = 32;
const uint32_t polynom = 0x4c11db7;
const int32_t direct = 1;
const uint32_t crcinit = 0xffffffff;
const uint32_t crcxor = 0xffffffff;
const int32_t refin = 1;
const int32_t refout = 1;
uint32_t crcinit_direct;
uint32_t crcmask;
uint32_t crchighbit;
uint32_t crcinit_nondirect;

uint32_t reflect (uint32_t crc, int32_t bitnum) {

	// reflects the lower 'bitnum' bits of 'crc'

	uint32_t i, j=1, crcout=0;

	for (i=(uint32_t)1<<(bitnum-1); i; i>>=1) {
		if (crc & i) crcout|=j;
		j<<= 1;
	}
	return (crcout);
}

uint32_t crcbitbybitfast(unsigned char* p, uint32_t len) {

	// fast bit by bit algorithm without augmented zero bytes.
	// does not use lookup table, suited for polynom orders between 1...32.

	uint32_t i, j, c, bit;
	uint32_t crc = crcinit_direct;

	//	printf("crcbitbybitfast entering\n");

	for (i=0; i<len; i++) {
		c = (uint32_t)*p++;
		//	printf("crc is %d\n", i);

		if (refin) c = reflect(c, 8);

		for (j=0x80; j; j>>=1) {

			bit = crc & crchighbit;
			crc<<= 1;
			if (c & j) bit^= crchighbit;
			if (bit) crc^= polynom;
		}
	}

	if (refout) crc=reflect(crc, order);
	crc^= crcxor;
	crc&= crcmask;
	//	printf("crcbitbybitfast exiting\n");

	return(crc);
}

uint32_t checkMaskEtc(){
	crcmask = ((((uint32_t )1 << (order - 1)) - 1) << 1) | 1;
	crchighbit = (uint32_t )1 << (order-1);

	return(0);
}





void recvFile(int fd, int *nargs, unsigned char **argv){

	unsigned char arduinoFileToSend[20];
	unsigned char hostFileToSaveAs[20];

	if (*nargs == 1){
		strcpy(arduinoFileToSend, "dummyFile");
		strcpy(hostFileToSaveAs, "dummyFile");
	} else if (*nargs == 2) {
		strcpy(arduinoFileToSend, argv[1]);
		strcpy(hostFileToSaveAs, argv[1]);
	} else {
		strcpy(arduinoFileToSend, argv[1]);
		strcpy(hostFileToSaveAs, argv[2]);
	}

	header recv;

	int32_t error = checkMaskEtc();

	unsigned char ch;

	printf("\n > local recvFile \n");
	int wlen, rlen;

	while(1) {
		read(fd, &ch, 1);
		if (ch == BOT) break;
	}
	wlen = write(fd, &BOT, 1);

	unsigned char *chr;
	chr = (unsigned char *)&recv;
	rlen = 0;
	int recved = 0;

	while(rlen < sizeof(header)){
		if (read(fd, chr, 1) > 0){
			rlen++;
			chr++;
		}
	}

	printf(" > local header rlen %d\n", rlen);
	printf(" > local header recv fileSize %d\n", recv.fileSize );
	printf(" > local header recv bufSize %d\n", recv.bufSize );
	printf(" > local header recv crcX %d\n", recv.crcX );
	printf(" > local header recv fileName %s\n", recv.fileName );
	printf(" > local header recv poly %d\n", recv.poly );
	printf(" > local header recv initX %d\n", recv.initX );
	printf(" > local header recv crcCheck %d\n", recv.crcCheck );

	int bufSize = recv.bufSize;
	int numFrames;
	int remainder;
	int fileSize = recv.fileSize;
	int crcSize = 4;
	uint32_t crc;
	uint32_t crcTmp;
	char filename[20];

	// check crc
	crcClcData.crcInt = recv.crcCheck;

	crcClcData.crcInt = crcbitbybitfast((unsigned char *)(&recv), sizeof(recv) - 4);

	strcpy(filename, hostFileToSaveAs);

	printf(" > local filename %s\n", filename );

	numFrames = fileSize/(bufSize - crcSize);
	remainder = fileSize % (bufSize - crcSize);
	printf(" > local numFrames %d\n", numFrames );
	printf(" > local remainder %d\n", remainder );

	//	unsigned char *ptr;
	int returnvalue;
	FILE *ptr_myfile;

	ptr_myfile = fopen(filename,"wb");

	// read and write the bulk
	int count = 0;

	for(int32_t j = 0; j < numFrames; j++) {
		if (j%10 == 0) printf("<local><010> : local frame is %d\n", j );

		count = 0;

		while(1) {
			wlen = write(fd, &EOT, 1);

			// read data from arduino
//			usleep(20000);
			for(int32_t i = 0; i < bufSize; i++){
				read(fd, &(oneKbuf[i]), 1);
			}
//			usleep(20000);
			// 	corrupting received data
			//		if (j == 10 && count ==0) oneKbuf[0] = 'a';
			//		 	for(int32_t i = 0; i < bufSize; i++){
			//				printf("%c",oneKbuf[i]);
			//			}

			crcClcData.crcInt = crcbitbybitfast(oneKbuf, bufSize - crcSize);

			for(int32_t i = 0; i < crcSize; i++){
				crcRcvData.crcArray[i] = oneKbuf[bufSize - crcSize + i];
			}

			/*		printf("<local><011> : oneKbuf[0] is %c \n", oneKbuf[0]);

		printf("<local><012> : local crcClcData is %x %x %x %x \n",
					crcClcData.crcArray[0],
					crcClcData.crcArray[1],
					crcClcData.crcArray[2],
					crcClcData.crcArray[3]);

		printf("<local><012> : local crcRcvData is %x %x %x %x \n",
					crcRcvData.crcArray[0],
					crcRcvData.crcArray[1],
					crcRcvData.crcArray[2],
					crcRcvData.crcArray[3]);	*/

//			usleep(20000);

			wlen = write(fd, &SYNC, 1);

			while(1) {
				read(fd, &ch, 1);
				if (ch == SYNC) break;
			}

			if (crcClcData.crcInt == crcRcvData.crcInt) {
				wlen = write(fd, &SOK, 1);
				break;
			} else {
				wlen = write(fd, &NOK, 1);
				count++;
				if (count == RETRYCOUNT) break;
			}
		} 	// infinite resend loop
		fwrite(oneKbuf, bufSize - crcSize, 1, ptr_myfile);
	}

	//	now do the remainder

	printf("<local> : Remainder \n");
	printf("<local> : --------- \n");

	count = 0;

	while(1) {
		wlen = write(fd, &EOT, 1);

		// read data from arduino
		usleep(20000);
		for(int32_t i = 0; i < remainder + crcSize; i++){
			read(fd, &(oneKbuf[i]), 1);
		}
		usleep(20000);
		// 	corrupting received data
		/*		if (j == 0 && count ==0) oneKbuf[0] = 'a';
		 	for(int32_t i = 0; i < bufSize; i++){
				printf("%c",oneKbuf[i]);
			}
		 */
		crcClcData.crcInt = crcbitbybitfast(oneKbuf, remainder);

		for(int32_t i = 0; i < crcSize; i++){
			crcRcvData.crcArray[i] = oneKbuf[remainder + i];
		}
		/*
		printf("<local><011> : oneKbuf[0] is %c \n", oneKbuf[0]);

		printf("<local><012> : local crcClcData is %x %x %x %x \n",
					crcClcData.crcArray[0],
					crcClcData.crcArray[1],
					crcClcData.crcArray[2],
					crcClcData.crcArray[3]);

		printf("<local><012> : local crcRcvData is %x %x %x %x \n",
					crcRcvData.crcArray[0],
					crcRcvData.crcArray[1],
					crcRcvData.crcArray[2],
					crcRcvData.crcArray[3]);	*/
		usleep(20000);

		wlen = write(fd, &SYNC, 1);

		while(1) {
			read(fd, &ch, 1);
			if (ch == SYNC) break;
		}

		if (crcClcData.crcInt == crcRcvData.crcInt) {
			wlen = write(fd, &SOK, 1);
			break;
		} else {
			wlen = write(fd, &NOK, 1);
			count++;
			if (count == RETRYCOUNT) break;
		}
	} 	// infinite resend loop
	fwrite(oneKbuf, remainder, 1, ptr_myfile);


	fclose(ptr_myfile);

	printf(" > local closing file\n");
	printf(" > local : ");

}

//*****************************


void sendFile(int fd, int *nargs, unsigned char **argv){
	unsigned char ch;
	printf("<local><sendFile><01> : sending file %s file name length %ld as ", argv[1], strlen(argv[1]));
	printf(" file %s file name length %ld \n", argv[2], strlen(argv[2]));
	//	read(fd, &send, sizeof(send));
	// copy args into local buffer as they are still in the oneKbuf

	unsigned char hostFileToSend[20];
	unsigned char ArduinoSaveAs[20];

	if (*nargs == 1){
		strcpy(hostFileToSend, "dummyFile");
		strcpy(ArduinoSaveAs, "dummyFile");
	} else if (*nargs == 2) {
		strcpy(hostFileToSend, argv[1]);
		strcpy(ArduinoSaveAs, argv[1]);
	} else {
		strcpy(hostFileToSend, argv[1]);
		strcpy(ArduinoSaveAs, argv[2]);
	}

	printf("<local><sendFile><02> : sending file %s file name length %ld as ", hostFileToSend, strlen(hostFileToSend));
	printf(" file %s save length %ld \n", ArduinoSaveAs, strlen(ArduinoSaveAs));

	write(fd, &BOT, 1);

	while(1) {
		//	 printf("<local><sendFile><02> : begin transmission\n");

		read(fd, &ch, 1);
		if (ch == BOT) break;
	}


	//		 printf("<local><sendFile><03> : done syncing\n");

	FILE *ptr_myfile;

	ptr_myfile = fopen(hostFileToSend,"rb");

	int fileSize;

	fseek(ptr_myfile, 0L, SEEK_END);
	fileSize = ftell(ptr_myfile);

	rewind(ptr_myfile);

	union crcOverlap {
		uint32_t crcInt;
		unsigned char crcArray[4];
	};

	union crcOverlap crcClcData;

	unsigned char *ptr;

	//create header

	header send;

	send.bufSize = bufSize;
	send.fileSize = fileSize;
	send.crcX = 365;
	strcpy(send.fileName, ArduinoSaveAs);
	send.poly = 7777;
	send.initX = 6666;
	send.crcCheck = 12345;

	int  wlen;

	crcClcData.crcInt = 4500;

	//	delay(100);
	wlen = write(fd, &send, sizeof(header));
	usleep(2000);

	printf("wlen = %d \n", wlen);

	int numFrames;
	int remainder;

	numFrames = fileSize/(bufSize - crcSize);
	remainder = fileSize % (bufSize - crcSize);

	printf(" > local numFrames %d\n", numFrames );
	printf(" > local remainder %d\n", remainder );


	// read and write the bulk
	for(int32_t j = 0; j < numFrames; j++) {
		printf("<local><sendFile> : frame is %d of %d\n", j, numFrames);

		// read data from file
		fread(oneKbuf, bufSize - crcSize, 1, ptr_myfile);
		//		printf("<local><sendFile> : frame read \n");

		crcClcData.crcInt = crcbitbybitfast(oneKbuf, bufSize - crcSize);

		for(int i = 0; i < crcSize; i++) oneKbuf[bufSize - crcSize + i] = crcClcData.crcArray[i];

		int count = 0;
		while (1) {
			//		printf("<local><sendFile><04> : trying to send frame\n");
			cleanUp(fd);

			wlen = write(fd, oneKbuf, bufSize);
			usleep(20000);
			//		usleep(1000);
			//	printf("<local><sendFile><05> : sent frame\n");
			//	cleanUp(fd);

			read(fd, &ch, 1);

			//		printf("<local><sendFile><06> : getting sync signal\n");

			if (ch == SYNC) {
				//				printf("<local><sendFile><02> : syncing\n");
				read(fd, &tmpCrc, 1);
				if (tmpCrc == SOK){
					//					printf("<local><sendFile><02> : crc code match: send ok\n");
					break;
				} else if (tmpCrc == NOK){
					count++;
					//					printf("<local><sendFile><02> : crc code no match: re send count number %d \n", count);

					if (count == 10) break;
				} else {
					printf("<local><sendFile><02> : dont recognize send code\n");
				}
			} else {
				printf("<local><sendFile><02> : syncing dont know\n");
			}
		}

		//	tcdrain(fd);    /* delay for output *



	}


	fread(oneKbuf, remainder, 1, ptr_myfile);

	crcClcData.crcInt = crcbitbybitfast(oneKbuf, remainder);

	for(int i = 0; i < crcSize; i++) oneKbuf[remainder + i] = crcClcData.crcArray[i];

	int count = 0;
	while (1) {
		write(fd, oneKbuf, remainder + crcSize);

		read(fd, &ch, 1);

		if (ch == SYNC) {
			printf("<local><sendFile><02> : syncing\n");
			read(fd, &tmpCrc, 1);
			if (tmpCrc == SOK){
				printf("<local><sendFile><03> : remainder crc code match: send ok\n");
				break;
			} else if (tmpCrc == NOK){
				count++;
				printf("<local><sendFile><03> : remainder crc code no match: re send count number %d \n", count);

				if (count == 10) break;
			} else {
				printf("<local><sendFile><02> : remainder dont recognize send code\n");
			}
		} else {
			printf("<local><sendFile><02> : remainder syncing dont know\n");
		}



	}
	printf("<local><sendFile> : sync complete %d \n", wlen);

	fclose(ptr_myfile);


	printf("<local><sendFile> : closing file afer sending file\n");
	printf("<local><sendFile> : ending");
}



void getArguments(unsigned char *keyBoardInput, int inputBufferSize, int *nargs, unsigned char *argv[10]){

	*nargs = 0;
	for(int i = 0; i < 10; i++) argv[i] = NULL;

	//	printf("<local><getArguments><01> : arg number %d \n", *nargs);

	if (keyBoardInput[0] != ' '){
		argv[0] = &keyBoardInput[0];
		(*nargs)++;
	}
	//	printf("<local><getArguments><02> : arg number %d \n", *nargs);

	for(int i = 0; i < inputBufferSize - 1; i++){
		if (keyBoardInput[i] == ' ' && keyBoardInput[i + 1] != ' '){
			argv[*nargs] = &keyBoardInput[i + 1];
			(*nargs)++;
		}
	}


	for(int i = 0; i < inputBufferSize; i++){
		if (keyBoardInput[i] == ' ' ||
				keyBoardInput[i] == '\r' ||
				keyBoardInput[i] == '\n'){
			keyBoardInput[i] = '\0';
		}
	}

	//	printf("<local><getArguments><03> : arg number %ld \n", strlen(argv[0]));
	//	printf("<local><getArguments><03> : arg number %d \n", *nargs);

	// now set end of strings with \0


	// set commands to uppercase
	for(unsigned char *j = argv[0]; j < (argv[0] + strlen((char *)argv[0])); j++){
		if (*j >= 'a' && *j <= 'z') *j = *j - 32;
	}
	//	printf("<local> : arg number %d \n", *nargs);

}

void listing(){
	struct dirent *de;  // Pointer for directory entry

	// opendir() returns a pointer of DIR type.
	DIR *dr = opendir(".");

	if (dr == NULL)  // opendir returns NULL if couldn't open directory
	{
		printf("Could not open current directory" );
		return;
	}

	while ((de = readdir(dr)) != NULL)
		printf("%s\n", de->d_name);

	closedir(dr);
}



int main()
{
	printf("> local : ");
	unsigned char keyBoardInput[inputBufferSize];
	char *portname = "/dev/ttyS5";
	int fd;
	int wlen;
	int32_t error = checkMaskEtc();

	fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
	if (fd < 0) {
		printf("Error opening %s: %s\n", portname, strerror(errno));
		return -1;
	}
	/*baudrate 115200, 8 bits, no parity, 1 stop bit */
	    set_interface_attribs(fd, 115200);
//	set_interface_attribs(fd, 57600);
	//   set_interface_attribs(fd, 9600);
	//   set_mincount(fd, 0);                /* set to pure timed read */
	set_blocking(fd, 0);
	//   sleep(10);
	unsigned char ch;

	/* simple output */
	char * pch;
	unsigned char *cmdStr;
	char *cmd;
	char *param;
	int len;

	/*
//unsigned char ch;
	while(1) {
//	printf("here \n");
		read(fd, &ch, 1);
		//if (ch == EOT) break;
			printf("%c", ch);
	}
	//		cleanUp(fd);
	 */
	while (1){
		for(int i = 0; i < inputBufferSize; i++ ) keyBoardInput[i] = '\n';

		// poll keyboard
		fgets(keyBoardInput, strlen((char *)keyBoardInput), stdin);

		// send tidied input to arduino

		printf("<local><main><01> : input length =  %ld \n", strlen((char *)keyBoardInput));

		wlen = write(fd, keyBoardInput, strlen((char *)keyBoardInput));

		//	cleanUp(fd);

		int nargs = 0;
		unsigned char *argv[10];
		getArguments(keyBoardInput, strlen((char *)keyBoardInput), &nargs, argv);
		printf("<local><main><02> : \n");


		if (!strcmp((char *)argv[0], "HELP")){
			printf("<local> : help\n");
			cleanUp(fd);
		} else if (!strcmp((char *)argv[0], "DIR")){
			printf("<local> : listing remote directory\n");
			cleanUp(fd);
		} else if (!strcmp((char *)argv[0], "LDIR")){
			printf("<local> : listing local directory\n");
			listing();
			printf("<local> : listing remote directory\n");
			cleanUp(fd);
		} else if (!strcmp((char *)argv[0], "ATOH")){
			printf("<local> : expecting file xx \n");
			printf("<local> : recvFile xxxx \n");
			recvFile(fd, &nargs, argv);
			cleanUp(fd);
		} else if (!strcmp((char *)argv[0], "HTOA")){
			printf("<local> : sending file\n");
			printf("<local> : send to arduino \n ");
			printf("<local> : sending file %s file name length %ld as ", argv[1], strlen(argv[1]));
			printf(" file %s file name length %ld \n", argv[2], strlen(argv[2]));
			sendFile(fd, &nargs, argv);
			cleanUp(fd);
		} else if (!strcmp((char *)argv[0], "QUIT")){
			printf("<local> : exiting \n");
			cleanUp(fd);
			return (0);
		} else  {
			printf("<local> : other command?\n");
			cleanUp(fd);
		}
		printf("<local> : ");

	}

	sleep(100);
}
