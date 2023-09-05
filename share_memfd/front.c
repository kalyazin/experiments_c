#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/memfd.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <poll.h>

static int SIZE = 4096;
static char* SERVER_SOCKET_PATH = "test_socket";

static void *pf_handler(void *args)
{
	int uffd = (int)args;
	long tid = syscall(__NR_gettid);
	static char data[4096];

	strncpy(data, "You should be seeing this!", sizeof(data));

	printf("[%ld] Starting page fault handler for UFFD: %d\n", tid, uffd);

	while (1) {
		struct pollfd pollfd = { .fd = uffd, .events = POLLIN };
		int nready = poll(&pollfd, 1, -1);
		if (nready == -1) {
			perror("[PF_handler] poll");
			exit(EXIT_FAILURE);
		} else if (nready == 0) {
			printf("[%ld] spurious wake up. continuing\n", tid);
			continue;
		}

		printf("[%ld] We have %d available page fault events\n", tid, nready);
		struct uffd_msg msg;

		int ret = read(uffd, &msg, sizeof(msg));
		if (ret != sizeof(msg)) {
			perror("[PF_handler] read UFFD event");
			exit(EXIT_FAILURE);
		}

		if (msg.event != UFFD_EVENT_PAGEFAULT) {
			printf("[%ld] unexpected event: %d\n", tid, msg.event);
			exit(EXIT_FAILURE);
		}

		printf("[%ld] Page-fault info\n", tid);
		printf("[%ld] flags: 0x%llx\n", tid, msg.arg.pagefault.flags);
		printf("[%ld] address: %p\n", tid, (void *)msg.arg.pagefault.address);
		printf("[%ld] thread: %d\n", tid, msg.arg.pagefault.feat.ptid);

		struct uffdio_copy uffdio_copy = {
			.dst = msg.arg.pagefault.address & ~(4095),
			.src = (unsigned long)data,
			.len = 4096,
			.mode = 0,
			.copy = 0
		};

		printf("[%ld] Serving page fault with: '%s'\n", tid, data);
		if (ioctl(uffd, UFFDIO_COPY, &uffdio_copy) == -1) {
			perror("[PF handler] ioctl(UFFDIO_COPY)");
			exit(EXIT_FAILURE);
		}
	}
}

int setup_uffd(void *addr, size_t size)
{
	long tid = syscall(__NR_gettid);
	/* Create the UFFD */
	int uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0) {
		perror("syscall(SYS_userfaultfd)");
		return -1;
	}
	printf("[%ld] Created UFFD: %d\n", tid, uffd);

	/* feature negotiation */
	struct uffdio_api uffdio_api;
	uffdio_api.api = UFFD_API;
	uffdio_api.features = 0;
	if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1) {
		perror("ioctl(UFFDIO_API)");
		return -1;
	}

	/* Register address range */
	struct uffdio_register uffdio_register;
	uffdio_register.range.start = (unsigned long)addr;
	uffdio_register.range.len = size;
	/* Just get events about a page missing */
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_MINOR;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
		perror("ioctl(UFFDIO_REGISTER)");
		return -1;
	}

	/* Spawn a new pthread to handle page faults */
	printf("[%ld] Starting page fault handler thread\n", tid);
	pthread_t handler;
	if (pthread_create(&handler, NULL, pf_handler, (void *)uffd)) {
		perror("pthread_create");
		return -1;
	}

	return 0;
}

int main() {
  int memfd = syscall(SYS_memfd_create, "memfd_test", 0);
  if (memfd < 0) {
    printf("memfd failed\n");
    return 1;
  }
  printf("memfd: %d\n", memfd);

  int r = ftruncate(memfd, SIZE);
  if (r < 0) {
    printf("memfd failed\n");
    return 1;
  }

  char* memfd_map = (char*)mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
  if (memfd_map == MAP_FAILED) {
    printf("memfd map failed\n");
    return 1;
  }
  printf("memfd_map: %p\n", memfd_map);
  //syscall(SYS_getrandom, (void *)memfd_map, SIZE, 0);

  if (setup_uffd(memfd_map, SIZE) < 0) {
  	printf("Could not setup UFFD\n");
  	return 1;
  }

  // create socket
  int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    printf("sockfd failed\n");
    return 1;
  }

  struct sockaddr_un server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, SERVER_SOCKET_PATH, strlen(SERVER_SOCKET_PATH));

  if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    printf("connect failed\n");
    return 1;
  }

  char iov_dummy = 'A';
  struct iovec iov = {
    .iov_base = &iov_dummy,
    .iov_len = sizeof(char),
  };
  char buff[CMSG_SPACE(sizeof(int))];
  struct msghdr msg = {
    .msg_name = 0,
    .msg_namelen = 0,
    .msg_iov = &iov,
    .msg_iovlen = 1,
    .msg_control = &buff,
    .msg_controllen = sizeof(buff),
    .msg_flags = 0,
  };

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  int* data = (int*)CMSG_DATA(cmsg);
  memcpy(data, &memfd, sizeof(int));

  printf("sending FD: %d\n", memfd);
  sendmsg(sockfd, &msg, 0);

  printf("sleeping\n");
  /*
  for (int i = 0; i < 10; i++) {
    sleep(1000);
  }
  */
  sleep(5);
  printf("[%ld]: Message: %s\n", syscall(__NR_gettid),  memfd_map);
  sleep(10);

  munmap(memfd_map, SIZE);
  close(memfd);
  close(sockfd);
}
