#define _GNU_SOURCE

#include <fcntl.h> // open 
#include <unistd.h> // unlink
#include <string.h> 
#include <stdio.h> // printf
#include <stdlib.h> // atoi

#include <malloc.h> // memalign 

#define BLK_SIZE 512

int main(int argc, char** argv)
{
  char fname[20] = "/dev/nvme0n1";
  //char fname[20] = "/dev/xvdb1";

  // offset, length, alignment should be a multiple of block size (512B) 
  ssize_t num;
  size_t length, alignment;
  off_t offset;
  char* buff;
  int ops, op_size;
  int count;

  length = BLK_SIZE * 8;
  alignment = BLK_SIZE;
  offset = 0;

  // file open 
  if (argc < 3){ 
    printf("Usage: ./a.out op_size ops\n");
    printf("e.g.: ./a.out 4096 1000\n");
    return 0;
  }

  op_size = atoi(argv[1]);  
  ops = atoi(argv[2]);

  printf("op_size = %d, ops = %d\n", op_size, ops);

  int fd, oflag; 

  oflag = O_CREAT | O_RDWR | O_DIRECT | O_DSYNC;
  //oflag = O_CREAT | O_RDWR;

  fd = open(fname, oflag, S_IRUSR | S_IWUSR);
  
  if (fd == -1){
    printf("Failed to open file %s\n", fname);
    return -1; 
  }

  // io
  buff = memalign(alignment * 2, length + alignment);

  if(buff == NULL){
    printf("Failed to memalign\n");
    goto out;
  }   

  buff += alignment; 
  memset(buff, 0, length);
  //memcpy(buff, "This is my direct file", 200);

  count = 0;

  while(count < ops){

	  if (lseek(fd, offset, SEEK_SET) == -1){
	    printf("Failed to lseek\n");
	    goto out;
	  }

	  num = write (fd, buff, length);
	  if (num == -1){
	    printf("Failed to write\n");
	    goto out;
	  }
	    count++;
  }

  printf("%ld bytes are written\n", num*ops);

out:
//  if(buff != NULL)
//    free(buff);

  close(fd);
  return 0;

}

