#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <getopt.h>
#include <glob.h>

#define RESPONSE \
"ICY 200 OK\r\n" \
"icy-notice1: <BR>This stream requires <a href=\"http://www.winamp.com/\">Winamp</a><BR>\r\n" \
"icy-notice2: Lars's Shoutcast server<BR>\r\n" \
"icy-name: %s\r\n" \
"icy-genre: %s\r\n" \
"icy-url: http://localhost:%s\r\n" \
"content-type: audio/mpeg\r\n" \
"icy-pub: 1\r\n" \
"icy-metaint: %d\r\n" \
"icy-br: 96\r\n\r\n" \

#define CHUNK_SIZE 24576

#define HEADER_BLOCK "StreamTitle='%s';StreamUrl='%s:%s';"
#define HEADER_BLOCK_CHUNK_SIZE 16
#define DEFAULT_PORT "3000"

/*
TODO:
* stats:
bytes_out
served songs
num clients
num_unique clients

play same stream to several clients (harder)

Makefile and autotools

parse GET and open that directory if no direcory specified

 */


static const struct option long_options[] = {
        {"max-clients", 1, NULL, 'm'},
	{"port", 1, NULL, 'p'},
	{"filelist", 1, NULL, 'f'},
	{"random", 0, NULL, 'r'},
	{"directory", 1 , NULL, 'd'},
	{"chunk_size", 1, NULL, 'c'},
        {"help", 0, NULL, 'h'},
        {NULL, 0, NULL, 0},
};

static const char *const short_options = "m:p:f:d:rh";

/* Should we play dir in random order? */
static char random_order = 0;
static char *directory = NULL;

void sigchld_handler(int s)
{
	while(waitpid(-1, NULL, WNOHANG) > 0);
}


FILE* get_next_file(char *directory, char *title)
{
	glob_t pglob;
	char pattern[256];
	FILE *file;
	static unsigned int cur_file;

	sprintf(pattern, "%s/*.mp3", directory);
	if (glob(pattern, 0, NULL, &pglob) || pglob.gl_pathc == 0) {
		globfree(&pglob);
		return NULL;
	}
	
	if (random_order)
		cur_file = rand() % pglob.gl_pathc;

	sprintf(title, "%s", pglob.gl_pathv[cur_file]);
	file =  fopen(pglob.gl_pathv[cur_file], "rb");
	
	if (!random_order) {
		if (cur_file == pglob.gl_pathc - 1)
			cur_file = 0;
		else
			cur_file++;
	}
	
	globfree(&pglob);
	
	return file;
}

int create_header_block(char *header_block, char *title, char *url, char *port)
{
	int header_block_len, num_header_blocks;
	int header_block_remainder, i;

	header_block_len = sprintf(header_block + 1, HEADER_BLOCK, title, url, port);

	num_header_blocks = (int)(header_block_len / HEADER_BLOCK_CHUNK_SIZE);
	header_block_remainder = header_block_len % HEADER_BLOCK_CHUNK_SIZE;
	header_block[0] = num_header_blocks;
	
	// If there is a remainder we need to 0-pad and increase num blocks by 1
	if (header_block_remainder != 0) {
		for(i = header_block_len + 1; i < header_block_len + header_block_remainder; i++)
			header_block[i] = '\0';
		num_header_blocks++;
		header_block[0] = num_header_blocks;
	}

	// Total size is number of blocks plus 1 for the first num blocks byte
	return num_header_blocks * HEADER_BLOCK_CHUNK_SIZE + 1;
}


void print_help_and_exit()
{
	fprintf(stderr, "shouts [-p port] [-m max_clients] [-h]\n");
	exit(0);
}


void do_client(int fd, char *port, int chunk_size)
{
	char buf[1024];
	char out_buf[1024];
	int num_bytes, out_len;
	FILE *cur_file;
	char *data_buf = (char *)malloc(chunk_size + 1024);
	int data_len = chunk_size, fragment_data_len;
	int header_block_len;
	char header_block[HEADER_BLOCK_CHUNK_SIZE * 8];
	char done = 0;
	char title[256];

	if ((num_bytes = recv(fd, buf, 1024-1, 0)) == -1) {
		perror("recv");
		exit(1);
	}

	out_len = sprintf(out_buf, RESPONSE, "shouts", "shouts", port, chunk_size);
	out_buf[out_len] = '\0';
	printf("%s\n", out_buf);

	buf[num_bytes] = '\0';
	
	printf("server received:\n%s\n:\n", buf);

	printf("sending:%s\n", out_buf);
	
	if (send(fd, out_buf, out_len, 0) == -1)
		perror("send");

	
	cur_file = get_next_file("mp3s", title);

	printf("done:%d\n", done);

	while(!done) {
		if (!cur_file) {
			printf("can't open file\n");
			return;
		}
		
		//sleep(1);

		data_len = fread(data_buf, 1, chunk_size, cur_file);

		//printf("read file\n");

		if (data_len == chunk_size) {
			header_block_len = create_header_block(header_block, title, "http://localhost", port);
			memcpy(&data_buf[data_len], header_block, header_block_len);

			int len = send(fd, data_buf, chunk_size + header_block_len, 0);
			//printf("len:%d\n", len);
			if (len == -1 ){
			  perror("send");
			  break;
			}
		} else {
			/* close the current file as it is now at the end, and get the next one */
			fclose(cur_file);
			cur_file = get_next_file("mp3s", title);
			fragment_data_len = fread(data_buf, 1, chunk_size - data_len, cur_file);
			data_len = data_len + fragment_data_len;
			header_block_len = create_header_block(header_block, title, "http://localhost", port);
			memcpy(&data_buf[data_len], header_block, header_block_len);
       
			if (send(fd, data_buf, chunk_size + header_block_len, 0) == -1) {
				perror("send");
				break;
			}
		}
	}
}


void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	//	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


int main(int argc, char *argv[])
{
	int sockfd, new_fd;  
	//struct addrinfo hints, *servinfo, *p;
	struct sockaddr_storage their_addr; 
	socklen_t sin_size;
	struct sigaction sa;
	int yes = 1;
	char s[100];
	int rv;
	char *port = NULL;
	int next_option, max_clients = 10, chunk_size = CHUNK_SIZE;

        do {
                next_option = getopt_long(argc, argv, short_options, long_options, NULL);
                switch (next_option) {
		case 'm':
			max_clients = atoi(optarg);
			break;
		case 'p':
			port = optarg;
			break;
		case 'r':
			random_order = 1;
			break;
		case 'd':
			directory = optarg;
			break;
		case 'c':
			chunk_size = atoi(optarg);
		case 'h':
			print_help_and_exit();
			break;
		default:
			if (next_option != -1)
				print_help_and_exit();
		}
	} while(next_option != -1);

	if (!port)
		port = DEFAULT_PORT;

	
	printf("starting shouts on port %s with %d max-clients using directory %s. chunk_size:%d\n", 
	       port, max_clients, directory ? directory : "by client request", chunk_size);


	sockfd = socket(PF_INET, SOCK_STREAM, 0);

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
	  perror("setsockopt");
	  exit(1);
	}

struct sockaddr_in my_addr;

my_addr.sin_family = AF_INET;
my_addr.sin_port = htons(3000); // short, network byte order
my_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
memset(my_addr.sin_zero, '\0', sizeof my_addr.sin_zero);

	if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof my_addr) == -1) {
	  close(sockfd);
	  perror("server: bind");
	  //continue;
	}

	/*
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; 
	
	if ((rv = getaddrinfo(NULL, port, hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}
*/

	/*
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			perror("server: socket");
			continue;
		}
	
		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
			perror("setsockopt");
			exit(1);
		}
	    
		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("server: bind");
			//continue;
		}
		//break;
		//}
		*/
	/*
	if (p == NULL)  {
		fprintf(stderr, "server: failed to bind\n");
		return 2;
	}

	freeaddrinfo(servinfo); 
	*/

	if (listen(sockfd, max_clients) == -1) {
		perror("listen");
		exit(1);
	}
    
	sa.sa_handler = sigchld_handler; 
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1) {
		perror("sigaction");
		exit(1);
	}
    
	printf("server: waiting for connections...\n");
    
	while(1) { 
		sin_size = sizeof their_addr;
		new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
		if (new_fd == -1) {
			perror("accept");
			exit(1);
		}
	    
		inet_ntop(their_addr.ss_family,
			  get_in_addr((struct sockaddr *)&their_addr),
			  s, sizeof s);
	    
		printf("server: got connection from %s\n", s);
	    
		if (!fork()) { 
			close(sockfd); 
			
			do_client(new_fd, port, chunk_size);
		}
		close(new_fd); 
	}
    
	return 0;
}
