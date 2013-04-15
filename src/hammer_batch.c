#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include "hammer_sched.h"
#include "hammer_batch.h"
#include "hammer_socket.h"
#include "hammer_config.h"
#include "hammer_memory.h"
#include "hammer_macros.h"
#include "hammer_sched.h"
#include "hammer_epoll.h"
#include "hammer.h"
//#include "../libgpucrypto/crypto_size.h"
//#include "../libgpucrypto/crypto_context.h"
//#include "../libgpucrypto/crypto_mem.h"
#include "crypto_size.h"
#include "crypto_context.h"
#include "crypto_mem.h"

//pthread_key_t worker_batch_struct;
extern hammer_config_t *config;

int hammer_batch_init()
{
	int res;
	uint32_t alloc_size = config->batch_buf_max_size +
		config->batch_job_max_num * AES_KEY_SIZE +
		config->batch_job_max_num * AES_IV_SIZE +
		config->batch_job_max_num * PKT_OFFSET_SIZE + // input buffer
		config->batch_job_max_num * PKT_LENGTH_SIZE +
		config->batch_job_max_num * HMAC_KEY_SIZE;

	hammer_batch_t *batch = hammer_sched_get_batch_struct();
	
	// Buf A
	batch->buf_A.output_buf = cuda_pinned_mem_alloc(config->batch_buf_max_size);
	batch->buf_A.input_buf = cuda_pinned_mem_alloc(alloc_size);

	batch->buf_A.aes_key_pos = config->batch_buf_max_size;
	batch->buf_A.ivs_pos = batch->buf_A.aes_key_pos + config->batch_job_max_num * AES_KEY_SIZE;
	batch->buf_A.pkt_offset_pos = batch->buf_A.ivs_pos + config->batch_job_max_num * AES_IV_SIZE;
	batch->buf_A.length_pos = batch->buf_A.pkt_offset_pos + config->batch_job_max_num * PKT_OFFSET_SIZE;
	batch->buf_A.hmac_key_pos = batch->buf_A.length_pos + config->batch_job_max_num * PKT_LENGTH_SIZE;

	batch->buf_A.job_list = hammer_mem_malloc(config->batch_job_max_num * sizeof(hammer_job_t));
	batch->buf_A.buf_size = config->batch_buf_max_size;
	batch->buf_A.buf_length = 0;
	batch->buf_A.job_num = 0;

	// Buf B
	batch->buf_B.output_buf = cuda_pinned_mem_alloc(config->batch_buf_max_size);
	batch->buf_B.input_buf = cuda_pinned_mem_alloc(alloc_size);
	batch->buf_B.aes_key_pos = config->batch_buf_max_size;
	batch->buf_B.ivs_pos = batch->buf_B.aes_key_pos + config->batch_job_max_num * AES_KEY_SIZE;
	batch->buf_B.pkt_offset_pos = batch->buf_B.ivs_pos + config->batch_job_max_num * AES_IV_SIZE;
	batch->buf_B.length_pos = batch->buf_B.pkt_offset_pos + config->batch_job_max_num * PKT_OFFSET_SIZE;
	batch->buf_B.hmac_key_pos = batch->buf_B.length_pos + config->batch_job_max_num * PKT_LENGTH_SIZE;

	batch->buf_B.job_list = hammer_mem_malloc(config->batch_job_max_num * sizeof(hammer_job_t));
	batch->buf_B.buf_size = config->batch_buf_max_size;
	batch->buf_B.buf_length = 0;
	batch->buf_B.job_num = 0;

	batch->cur_buf = &(batch->buf_A);
	batch->cur_buf_id = 0;

	batch->processed_buf_id = -1;
	batch->buf_has_been_taken = -1;
	
	res = pthread_mutex_init(&(batch->mutex_batch_complete), NULL);
	if (res != 0) {
		perror("Mutex initialization failed");
		exit(EXIT_FAILURE);
	}

	res = pthread_mutex_init(&(batch->mutex_batch_launch), NULL);
	if (res != 0) {
		perror("Mutex initialization failed");
		exit(EXIT_FAILURE);
	}

	return 0;
}

uint64_t swap64(uint64_t v)
{
	return	((v & 0x00000000000000ffU) << 56) |
			((v & 0x000000000000ff00U) << 48) |
			((v & 0x0000000000ff0000U) << 24) |
			((v & 0x00000000ff000000U) << 8)  |
			((v & 0x000000ff00000000U) >> 8)  |
			((v & 0x0000ff0000000000U) >> 24) |
			((v & 0x00ff000000000000U) >> 48) |
			((v & 0xff00000000000000U) >> 56);
}

int hammer_batch_job_add(hammer_connection_t *c, int length)
{
	hammer_batch_t *batch = hammer_sched_get_batch_struct();
	int pad_length, job_num = batch->cur_buf->job_num;
	hammer_job_t *new_job = &(batch->cur_buf->job_list[job_num]);
	hammer_batch_buf_t *buf;
	void *base;

	/* Calculating the information for output buffer */
	new_job->job_body_ptr = batch->cur_buf->output_buf + batch->cur_buf->buf_length;

	/* Pad Sha-1
	 * 1) data must be padded to 512 bits (64 bytes)
	 * 2) Padding always begins with one-bit 1 first (1 bytes)
	 * 3) Then zero or more bits "0" are padded to bring the length of 
	 *    the message up to 64 bits fewer than a multiple of 512.
	 * 4) Appending Length (8 bytes). 64 bits are appended to the end of the padded 
	 *    message to indicate the length of the original message in bytes
	 *    4.1) the length of the message is stored in big-endian format
	 *    4.2) Break the 64-bit length into 2 words (32 bits each).
	 *         The low-order word is appended first and followed by the high-order word.
	 *--------------------------------------------------------
	 *    data   |100000000000000000000000000000000Length|
	 *--------------------------------------------------------
	 */
	pad_length = (length + 63 + 9) & (~0x3F);
	*(uint8_t *)(new_job->job_body_ptr + length) = 1 << 7;
	uint64_t len64 = swap64((uint64_t)length);
	*(uint64_t *)(new_job->job_body_ptr + pad_length - 8) = len64;

	/* Point this job ptr to the output buffer
	 * Although it has not been generated by GPU yet =P
	 * pad 16 for AES
	 * just now it is 64-byte aligned, SHA1_OUTPUT_SIZE = 20 bytes
	 * so there needs another 12 bytes for 16-alignment
	 */
	pad_length = (pad_length + SHA1_OUTPUT_SIZE + 16) & (~0x0F);
	new_job->job_body_length = pad_length;
	new_job->job_actual_length = length;
	new_job->connection = c;

	/* Add the job to the connection job list, so that it can be 
	 * forwarded in hammer_handler_write
	 */
	hammer_list_add(&(new_job->_head), c->job_list);

	buf = batch->cur_buf;
	/* Add aes_key to the input buffer */
	base = buf->input_buf + buf->aes_key_pos;
	memcpy((uint8_t *)base + AES_KEY_SIZE * job_num, c->aes_key, AES_KEY_SIZE);
	/* iv */
	base = buf->input_buf + buf->ivs_pos;
	memcpy((uint8_t *)base + AES_IV_SIZE * job_num, c->iv, AES_IV_SIZE);
	/* pkt_offset */
	base = buf->input_buf + buf->pkt_offset_pos;
	((uint32_t *)base)[job_num] = batch->cur_buf->buf_length;
	/* length */
	base = buf->input_buf + buf->length_pos;
	((uint16_t *)base)[job_num] = length;
	/* hmac key */
	base = buf->input_buf + buf->hmac_key_pos;
	memcpy((uint8_t *)base + HMAC_KEY_SIZE * job_num, c->hmac_key, HMAC_KEY_SIZE);
	/* Copy the RTSP header to its corresponding output buffer */

	/* calculate pkt_header_length
	  --- this is done in GPU ---
	pkt_header_length = 

	base = buf->output_buf;
	memcpy((uint8_t *)base + batch->cur_buf->buf_length, 
		(uint8_t *)(buf->input_buf) + batch->cur_buf->buf_length,
		pkt_header_length);
	*/

	/* Update batch parameters */
	batch->cur_buf->buf_length += pad_length;
	batch->cur_buf->job_num ++;

	if (batch->cur_buf->buf_length >= batch->cur_buf->buf_size ||
			batch->cur_buf->job_num >= config->batch_job_max_num) {
		hammer_err("error in batch job add\n");
		exit(0);
	}

	return 0;
}

/* We don't batch read from clients, which all need decryption.
   And these are supposed to be only requests, therefore we do not bother GPU to handle this
   while CPU is competent for fast AES operation with small amount of data -- AES-NI 

   However, we batch the read from server, since they all need encryption
*/
int hammer_batch_handler_read(hammer_connection_t *c)
{
	int recv_len, available;
	hammer_batch_t *batch = hammer_sched_get_batch_struct();

//			hammer_epoll_state_set(sched->epoll_fd, socket,
//					HAMMER_EPOLL_READ,
//					HAMMER_EPOLL_LEVEL_TRIGGERED,
//					(EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLIN));

	/* we batch encryption */
	if (c->type != HAMMER_CONN_SERVER) {
		hammer_err("this should be a connection with server\n");
		exit(0);
	}

	/* If GPU worker has processed the data */
	if (batch->processed_buf_id != -1) {
		hammer_batch_forwarding(batch);
	}

	/* Lock, we do not permit GPU worker to enter */
	///////////////////////////////////////////////////////////
	pthread_mutex_lock(&(batch->mutex_batch_launch));

	/* If GPU worker has fetched the data,
	 * we will switch our buffer, a two-buffer strategy. */ 
	if (batch->buf_has_been_taken != -1) {
		/* This buf has been taken */
		assert(batch->buf_has_been_taken == batch->cur_buf_id);
		hammer_batch_switch_buffer(batch);
	}
	
	available = batch->cur_buf->buf_size - batch->cur_buf->buf_length;
	if (available <= 0) {
		hammer_err("small available buffer!\n");
		exit(0);
	}

	/* Read incomming data */
	recv_len = hammer_socket_read (
			c->socket,
			batch->cur_buf->input_buf + batch->cur_buf->buf_length,
			available);
	if (recv_len <= 0) {
		// FIXME
		//if (errno == EAGAIN) {
		//	return 1;
		//} else {
			//hammer_session_remove(socket);
			hammer_err("read unencrypted, Hey!!!\n");
			exit(0);
		//}
	}

	/* Batch this job */
	hammer_batch_job_add(c, recv_len);

	/* Unlock, Now gpu worker has completed this read, GPU can launch this batch */
	///////////////////////////////////////////////////////////
	pthread_mutex_unlock(&(batch->mutex_batch_launch));

	/* check its rc  */
	if (c->rc == NULL) {
		hammer_err("This rc is considered to be existed \n");
		exit(0);
	}

	return 0;
}

/* This function trigger write event of all the jobs in this batch */
int hammer_batch_forwarding()
{
	int i;
	hammer_connection_t *rc;
	hammer_job_t *this_job;
	hammer_batch_buf_t *buf;
	hammer_sched_t *sched = hammer_sched_get_sched_struct();
	hammer_batch_t *batch = hammer_sched_get_batch_struct(); 

	assert(batch->processed_buf_id == (batch->cur_buf_id ^ 0x1));
	/* Get the buf that has been processed by GPU */
	if (batch->processed_buf_id == 0) {
		buf = &(batch->buf_A);
	} else {
		buf = &(batch->buf_B);
	}

	/* Set each connection to forward */
	for (i = 0; i < buf->job_num; i ++) {
		this_job = &(buf->job_list[i]);
		rc = this_job->connection->rc;
		
		hammer_epoll_change_mode(sched->epoll_fd,
				rc->socket,
				HAMMER_EPOLL_WRITE,
				HAMMER_EPOLL_LEVEL_TRIGGERED);
	}

	/* FIXME: are they really forwarded? use triple buffer! Mark this event as processed */
	//pthread_mutex_lock(&(batch->mutex_batch_complete));
	batch->processed_buf_id = -1;
	//pthread_mutex_unlock(&(batch->mutex_batch_complete));

	return 0;
}

int hammer_batch_switch_buffer()
{
	hammer_batch_t *batch = hammer_sched_get_batch_struct(); 

	if (batch->cur_buf_id == 0) {
		batch->cur_buf = &(batch->buf_B);
		batch->cur_buf_id = 1;
	} else {
		batch->cur_buf = &(batch->buf_A);
		batch->cur_buf_id = 0;
	}

	/* mark this event has been processed, and buf is switched*/
	batch->buf_has_been_taken = -1;

	/* refresh cur_buf  */
	batch->cur_buf->job_num = 0;
	batch->cur_buf->buf_length = 0;

	return 0;
}

int hammer_batch_if_gpu_processed_new()
{
	hammer_batch_t *batch = hammer_sched_get_batch_struct(); 

	if (batch->processed_buf_id == -1) {
		return 0;
	} else {
		return 1;
	}
}
