/*
 * Copyright (c) 2015 Justin Liu
 * Author: Justin Liu <rssnsj@gmail.com>
 * https://github.com/rssnsj/minivtun
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/md5.h>

#include "library.h"

struct name_cipher_pair {
	const char *name;
	const EVP_CIPHER *(*cipher)(void);
};

static struct name_cipher_pair cipher_pairs[] = {
	{ "aes-128", EVP_aes_128_cbc, },
	{ "aes-256", EVP_aes_256_cbc, },
	{ "des", EVP_des_cbc, },
	{ "desx", EVP_desx_cbc, },
	{ "rc4", EVP_rc4, },
};

const void *get_crypto_type(const char *name)
{
	const EVP_CIPHER *cipher = NULL;
	int i;

	for (i = 0; i < countof(cipher_pairs); i++) {
		if (strcasecmp(cipher_pairs[i].name, name) == 0) {
			cipher = cipher_pairs[i].cipher();
			break;
		}
	}

	if (cipher) {
		assert(EVP_CIPHER_key_length(cipher) <= CRYPTO_MAX_KEY_SIZE);
		assert(EVP_CIPHER_iv_length(cipher) <= CRYPTO_MAX_BLOCK_SIZE);
		return cipher;
	} else {
		return NULL;
	}
}

static const char crypto_ivec_initdata[CRYPTO_MAX_BLOCK_SIZE] = {
	0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
	0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
	0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
	0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
};

#define CRYPTO_DATA_PADDING(data, dlen, bs) \
	do { \
		size_t last_len = *(dlen) % (bs); \
		if (last_len) { \
			size_t padding_len = bs - last_len; \
			memset((char *)data + *(dlen), 0x0, padding_len); \
			*(dlen) += padding_len; \
		} \
	} while(0)

void datagram_encrypt(const void *key, const void *cptype, void *in,
		void *out, size_t *dlen)
{
	size_t iv_len = EVP_CIPHER_iv_length(cptype);
	EVP_CIPHER_CTX ctx;
	unsigned char iv[CRYPTO_MAX_KEY_SIZE];
	int outl = 0, outl2 = 0;

	if (iv_len == 0)
		iv_len = 16;

	memcpy(iv, crypto_ivec_initdata, iv_len);
	CRYPTO_DATA_PADDING(in, dlen, iv_len);
	EVP_CIPHER_CTX_init(&ctx);
	assert(EVP_EncryptInit_ex(&ctx, cptype, NULL, key, iv));
	EVP_CIPHER_CTX_set_padding(&ctx, 0);
	assert(EVP_EncryptUpdate(&ctx, out, &outl, in, *dlen));
	assert(EVP_EncryptFinal_ex(&ctx, (unsigned char *)out + outl, &outl2));
	EVP_CIPHER_CTX_cleanup(&ctx);

	*dlen = (size_t)(outl + outl2);
}

void datagram_decrypt(const void *key, const void *cptype, void *in,
		void *out, size_t *dlen)
{
	size_t iv_len = EVP_CIPHER_iv_length(cptype);
	EVP_CIPHER_CTX ctx;
	unsigned char iv[CRYPTO_MAX_KEY_SIZE];
	int outl = 0, outl2 = 0;

	if (iv_len == 0)
		iv_len = 16;

	memcpy(iv, crypto_ivec_initdata, iv_len);
	CRYPTO_DATA_PADDING(in, dlen, iv_len);
	EVP_CIPHER_CTX_init(&ctx);
	assert(EVP_DecryptInit_ex(&ctx, cptype, NULL, key, iv));
	EVP_CIPHER_CTX_set_padding(&ctx, 0);
	assert(EVP_DecryptUpdate(&ctx, out, &outl, in, *dlen));
	assert(EVP_DecryptFinal_ex(&ctx, (unsigned char *)out + outl, &outl2));
	EVP_CIPHER_CTX_cleanup(&ctx);

	*dlen = (size_t)(outl + outl2);
}

void fill_with_string_md5sum(const char *in, void *out, size_t outlen)
{
	char *outp = out, *oute = outp + outlen;
	MD5_CTX ctx;

	MD5_Init(&ctx);
	MD5_Update(&ctx, in, strlen(in));
	MD5_Final(out, &ctx);

	/* Fill in remaining buffer with repeated data. */
	for (outp += 16; outp < oute; outp += 16) {
		size_t bs = (oute - outp >= 16) ? 16 : (oute - outp);
		memcpy(outp, out, bs);
	}
}

/* =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */

int v4pair_to_sockaddr(const char *pair, char sep, struct sockaddr_in *addr)
{
	char host[64], *portp;
	struct addrinfo hints, *result;
	int rc;

	/* Only getting an INADDR_ANY address. */
	if (pair == NULL) {
		addr->sin_family = AF_INET;
		addr->sin_addr.s_addr = 0;
		addr->sin_port = 0;
		return 0;
	}

	strncpy(host, pair, sizeof(host));
	host[sizeof(host) - 1] = '\0';

	if (!(portp = strchr(host, sep)))
		return -EINVAL;
	*(portp++) = '\0';

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;  /* For wildcard IP address */
	hints.ai_protocol = 0;        /* Any protocol */
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;
	if ((rc = getaddrinfo(host, portp, &hints, &result)))
		return -EAGAIN;

	/* Get the first resolution. */
	*addr = *(struct sockaddr_in *)result->ai_addr;
	freeaddrinfo(result);
	return 0;
}

int do_daemonize(void)
{
	pid_t pid;

	if ((pid = fork()) < 0) {
		fprintf(stderr, "*** fork() error: %s.\n", strerror(errno));
		return -1;
	} else if (pid > 0) {
		/* In parent process */
		exit(0);
	} else {
		/* In child process */
		int fd;
		setsid();
		chdir("/tmp");
		if ((fd = open("/dev/null", O_RDWR)) >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
	}
	return 0;
}


