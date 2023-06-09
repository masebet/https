#include <thread>
#include <iostream>
#include <openssl/ssl.h>
#include "httpd.h"
#include "ZlHttpsSocket.h"

#define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#define FD_CNXCLOSED    0
#define FD_NODATA       -1
#define FD_STALLED      -2

static const int PROBE_AGAIN = -2;
static int https_sock = -1;
static int https_port = 443;

enum connection_state {
    ST_PROBING = 1,		/* Waiting for timeout to find where to forward */
    ST_SHOVELING		/* Connexion is established */
};

/* A 'queue' is composed of a file descriptor (which can be read from or
 * written to), and a queue for deferred write data */
struct queue {
    int fd;
    void *begin_deferred_data;
    void *deferred_data;
    int deferred_data_size;
};

struct connection {
    enum connection_state state;
    time_t probe_timeout;
    /* q[0]: queue for external connection (client);
     * q[1]: queue for internal connection (httpd or sshd);
     * */
    struct queue q[2];
};

int is_tls_protocol(const char *p, int len)
{
    if (len < 3)
	return PROBE_AGAIN;
    /* TLS packet starts with a record "Hello" (0x16), followed by version
     * (0x03 0x00-0x03) (RFC6101 A.1)
     * This means we reject SSLv2 and lower, which is actually a good thing (RFC6176)
     */
    return p[0] == 0x16 && p[1] == 0x03 && (p[2] >= 0 && p[2] <= 0x03);
}

/* Store some data to write to the queue later */
int defer_write(struct queue *q, void *data, int data_size)
{
    char *p;

    p = (char *) realloc(q->begin_deferred_data,
			 q->deferred_data_size + data_size);
    if (!p) {
	    perror("realloc");
	    exit(1);
    }

    q->deferred_data = q->begin_deferred_data = p;
    p += q->deferred_data_size;
    q->deferred_data_size += data_size;
    memcpy(p, data, data_size);

    return 0;
}

int probe_client_protocol(struct connection *cnx)
{
    char buffer[BUFSIZ];
    int n = read(cnx->q[0].fd, buffer, sizeof(buffer));

#ifdef DEBUG
    printf("in probe_client_protocol n is %d and buffer is %s\n",n, buffer);
#endif
    /* It's possible that read() returns an error, e.g. if the client
     * disconnected between the previous call to select() and now. If that
     * happens, we just connect to the default protocol so the caller of this
     * function does not have to deal with a specific  failure condition (the
     * connection will just fail later normally). */
    if (n > 0) {
	    int res = 0;
	    defer_write(&cnx->q[1], buffer, n);
	return res =
	    is_tls_protocol((char *) cnx->q[1].begin_deferred_data,
			    cnx->q[1].deferred_data_size);
    }
    return 0;
}

void init_cnx(struct connection *cnx)
{
    memset(cnx, 0, sizeof(*cnx));
    cnx->q[0].fd = -1;
    cnx->q[1].fd = -1;
}

/* tries to flush some of the data for specified queue
 * Upon success, the number of bytes written is returned.
 * Upon failure, -1 returned (e.g. connexion closed)
 * */
int flush_deferred(struct queue *q)
{
    int n = write(q->fd, q->deferred_data, q->deferred_data_size);
    if (n == -1)
	return n;

    if (n == q->deferred_data_size) {
	    /* All has been written -- release the memory */
	    free(q->begin_deferred_data);
	    q->begin_deferred_data = NULL;
	    q->deferred_data = NULL;
	    q->deferred_data_size = 0;
    } else {
	    /* There is data left */
	    q->deferred_data = (char *)q->deferred_data + n; //make compiler happy
	    q->deferred_data_size -= n;
    }

    return n;
}

/*
 * moves data from one fd to other
 *
 * returns number of bytes copied if success
 * returns 0 (FD_CNXCLOSED) if incoming socket closed
 * returns FD_NODATA if no data was available
 * returns FD_STALLED if data was read, could not be written, and has been
 * stored in temporary buffer.
 */
int fd2fd(struct queue *target_q, struct queue *from_q)
{
    char buffer[BUFSIZ];
    int target, from, size_r, size_w;

    target = target_q->fd;
    from = from_q->fd;

    size_r = read(from, buffer, sizeof(buffer));
    if (size_r == -1) {
	switch (errno) {
	case EAGAIN:
	    return FD_NODATA;

	case ECONNRESET:
	case EPIPE:
	    return FD_CNXCLOSED;
	}
    }



    if (size_r == 0)
	return FD_CNXCLOSED;

    size_w = write(target, buffer, size_r);
    /* process -1 when we know how to deal with it */
    if (size_w == -1) {
	switch (errno) {
	case EAGAIN:
	    /* write blocked: Defer data */
	    defer_write(target_q, buffer, size_r);
	    return FD_STALLED;

	case ECONNRESET:
	case EPIPE:
	    /* remove end closed -- drop the connection */
	    return FD_CNXCLOSED;
	}
    } else if (size_w < size_r) {
	/* incomplete write -- defer the rest of the data */
	defer_write(target_q, buffer + size_w, size_r - size_w);
	return FD_STALLED;
    }

    return size_w;
}

/* Connect to first address that works and returns a file descriptor, or -1 if
 * none work.
 * If transparent proxying is on, use fd_from peer address on external address
 * of new file descriptor. */
int connect_addr(int port)
{

    struct sockaddr_in localserver;
    memset(&localserver, 0, sizeof(localserver));	/* Clear struct */
    localserver.sin_family = AF_INET;	/* Internet/IP */
    localserver.sin_addr.s_addr = inet_addr("127.0.0.1");	/* IP address */
    localserver.sin_port = htons(port);	/* server port */

    int fd = socket(PF_INET, SOCK_STREAM, 0);
    int res;
    if (fd == -1) {
	exit(1);
    } else {
	res =
	    connect(fd, (struct sockaddr *) &localserver,
		    sizeof(localserver));
	if (res == -1) {
	    close(fd);
	} else {
	    return fd;
	}
    }
    return -1;
}

//TODO:  解密然后转发
void httpsServer(int sock_fd)
{
    struct sockaddr_in client_name;
    socklen_t client_name_len = sizeof(client_name);

    SSL_CTX *ctx = initSSL("ssl/my.cert", "ssl/my.key");
    while (1) {
	ZlHttpsSocket zlHttpsSocket(ctx);
	ZlSocket *sock = &zlHttpsSocket;
	if (sock->accept(sock_fd, (struct sockaddr *) &client_name,&client_name_len) != -1) {
	    accept_request(sock);
	    sock->close();
	    }
    }
    close(sock_fd);
    SSL_CTX_free(ctx);
}

int main(void)
{
    https_sock = -1;
    u_short port = https_port;
    https_sock = startup(&port);
    printf("https running on port %d\n", port);

    std::thread httpsThraed(httpsServer, https_sock);
    
    httpsThraed.join();
    return (0);
}
