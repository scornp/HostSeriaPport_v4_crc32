#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	printf("eerialport v4 crc32\n");
	char *portname;
	int speed;
	if (argc == 1){
		printf("eerialport using defaults /dev/ttyS5 speed 57600\n");
        *portname = '/dev/ttyS5';
		    speed = 57600;	
	) else if (argc == 3) {
		*portname = argv[1];
			speed = atoi(argv[2]);;	
	} else {
		printf("error unknown input\n");
		return -1;
	}
	
	printf("Opening port %s baud %d \n", portname, speed);

	return 0;
}