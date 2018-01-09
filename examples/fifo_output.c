/* Output callback test that uses file output with two memory buffers. Simpler than a FIFO
 * The OS is likely to already do some kind of buffering, but this method can give extra guarantees.
 *
 * The goal would be to offer a number of possible behaviors :
 * - uniform latency : drop log messages if buffer is overrun, but never block, waiting for buffer flush (slow IO )
 * - lazy best effort : low latency unless buffer is overrun, in which case the logging call will block waiting for buffer flush
 *
 * This example uses the "lazy best effort" method.  This does add an additional failure point, if for any reason
 * the buffer flush call doesn't complete, user code will block inside the logging call.
 *
 * 2017 - fenugrec
 * based on zf_log "output_callback.c" example,
 */

#define FIFO_SIZE	(8*1024)



#include <assert.h>
#if defined(_WIN32) || defined(_WIN64)
	#include <windows.h>
#endif
#include <zf_log.h>


#include <stdio.h>
#include <stdint.h>

typedef int8_t i8;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t u32;
typedef unsigned int uint;
typedef unsigned long ulong;


/************ mutex wrappers */
typedef void diag_mtx;

static 
diag_mtx *diag_os_newmtx(void) {
	CRITICAL_SECTION *lpc;
	lpc = malloc(sizeof(CRITICAL_SECTION));
	InitializeCriticalSection(lpc);
	return (diag_mtx *) lpc;
}

void diag_os_delmtx(diag_mtx *mtx) {
	CRITICAL_SECTION *lpc = (CRITICAL_SECTION *) mtx;
	DeleteCriticalSection(lpc);
	free(lpc);
	return;
}

void diag_os_lock(diag_mtx *mtx) {
	CRITICAL_SECTION *lpc = (CRITICAL_SECTION *) mtx;
	EnterCriticalSection(lpc);
	return;
}

void diag_os_unlock(diag_mtx *mtx) {
	CRITICAL_SECTION *lpc = (CRITICAL_SECTION *) mtx;
	LeaveCriticalSection(lpc);
	return;
}


struct fifo_t {
	diag_mtx *mtx;
	volatile uint rp;
	volatile uint wp;	//always 1 write ptr
	uint siz;
	uint used;
	u8 *data;
};

/************* FIFO stuff
 * mutex for everything
*/


/* assumes newmtx doesn't fail
 */
static void init_fifo(struct fifo_t *fifo, unsigned siz, u8 *rawbuf) {
	fifo->mtx = diag_os_newmtx();
	fifo->rp = 0;
	fifo->wp = 0;
	fifo->siz = siz;
	fifo->used = 0;
	fifo->data = rawbuf;
	return;
}

static void del_fifo(struct fifo_t *fifo) {
	diag_os_delmtx(fifo->mtx);
	return;
}


//return free space
#if 0
static uint fifo_rlen(struct fifo_t *fifo) {
	diag_os_lock(fifo->mtx);
	return (fifo->siz - fifo->used);
	diag_os_unlock(fifo->mtx);
}
#endif

//ret available data.
static uint fifo_used(struct fifo_t *fifo) {
	diag_os_lock(fifo->mtx);
	return fifo->used;
	diag_os_unlock(fifo->mtx);
}

//write block : write len bytes; advance ptr && ret len if success. Locks a lot
//ret 0 if incomplete
static uint fifo_wblock(struct fifo_t *fifo, u8 *src, const uint len) {
	uint wp;	//temp indexer
	uint siz;
	uint done = 0;

	siz = fifo->siz;

	diag_os_lock(fifo->mtx);

	if ((fifo->siz - fifo->used) < len) {
		//Not enough room : drop data
		done = 0;
		goto exit;
	}

	wp = fifo->wp;

	//bytewise copy. Slow but avoids having to split memcpy calls
	for (done=0; done < len; done++) {
		fifo->data[wp] = *src;
		wp++;
		src++;
		if (wp == siz) wp=0;
	}

	fifo->wp = wp;
	fifo->used += len;

exit:
	diag_os_unlock(fifo->mtx);
	return done;
}



/** return length if successful, 0 if requested too much
*/
static uint fifo_rblock(struct fifo_t *fifo, u8 *dest, const uint len) {
	uint done = 0;
	uint siz, rp;

	siz = fifo->siz;
	diag_os_lock(fifo->mtx);
	
	rp = fifo->rp;

	if (fifo->used < len) {
		//not enough data available : do nothing
		done = 0;
		goto exit;
	}

	for (done=0; done < len; done++) {
		*dest = fifo->data[rp];
		dest++;
		rp++;
		if (rp == siz) rp=0;
	}

	fifo->rp = rp;	//should probably have a mem barrier...
	fifo->used -= len;
exit:
	diag_os_unlock(fifo->mtx);
	return done;
}




struct membuf_t {
	FILE *out_file;
	struct fifo_t fifo;
};



#define MEMBUF_CHUNKS 4*1024
void membuf_flush(struct membuf_t *membuf) {
	u8 tempbuf[MEMBUF_CHUNKS];
	uint remsiz;

	remsiz = fifo_used(&membuf->fifo);
	while (remsiz) {
		uint oneshot = remsiz;
		if (oneshot > MEMBUF_CHUNKS) oneshot = MEMBUF_CHUNKS;

		if (fifo_rblock(&membuf->fifo, tempbuf, oneshot) != oneshot) {
			fprintf(membuf->out_file, "\nFATAL : could not read fifo !\n");
			return;
		}
		if (fwrite(tempbuf, 1, oneshot, membuf->out_file) != oneshot) {
			fprintf(membuf->out_file, "\nFATAL : fwrite problem !\n");
			return;
		}
		printf("fc\n");
		remsiz -= oneshot;
	}
	return;
}

static void custom_output_callback(const zf_log_message *msg, void *arg)
{
	struct membuf_t *membuf = arg;
	size_t msglen;

	*msg->p = '\n';

	msglen = (msg->p - msg->buf) + 1;
	if (fifo_wblock(&membuf->fifo, (u8 *) msg->buf, msglen) != msglen) {
		//dropped data !
		return;
	}
	printf("dbg: len=%zu\n", msglen);
	membuf_flush(membuf);
	return;
	
}

int main(int argc, char *argv[]) {
	FILE *outf;
	struct membuf_t membuf;
	uint8_t fifobuf[FIFO_SIZE];

	outf = fopen("test.log", "wb");
	if (!outf) {
		printf("can't open outfile\n");
		return -1;
	}

	membuf.out_file = outf;
	init_fifo(&membuf.fifo, FIFO_SIZE, fifobuf);

	zf_log_set_output_v(ZF_LOG_PUT_STD, &membuf, custom_output_callback);

	ZF_LOGI("argc=%i", argc);
	ZF_LOGI_MEM(argv, argc * sizeof(*argv), "and argv pointers as well:");

	membuf_flush(&membuf);
	del_fifo(&membuf.fifo);
	fclose(outf);
	return 0;
}
