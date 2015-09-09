#include <unistd.h>
#include <fcntl.h>
#include "common.h"
#include "gncEncoder.h"
#include "gncOADecoder.h"
extern void print_code_summary(struct gnc_metainfo *meta, int overhead, long operations);

char usage[] = "usage: ./test.OAdecoder datasize size_b size_g size_p";
int main(int argc, char *argv[])
{
	if (argc != 5) {
		printf("%s\n", usage);
		exit(1);
	}
	long datasize = atoi(argv[1]);
	int  size_b   = atoi(argv[2]);
	int  size_g   = atoi(argv[3]);
	int  size_p   = atoi(argv[4]);
	int  gnc_type = RAND_GNC_CODE;

	srand( (int) time(0) );
	char *buf = malloc(datasize);
	int rnd=open("/dev/urandom", O_RDONLY);
	read(rnd, buf, datasize);
	close(rnd);
	struct gnc_context *gc;

	// Construct a GNC code (32, 40, 1024)
	if (create_gnc_context(buf, datasize, &gc, size_b, size_g, size_p, gnc_type) != 0) {
		printf("Cannot create File Context.\n");
		return 1;
	}

	struct decoding_context_OA *dec_ctx = malloc(sizeof(struct decoding_context_OA));
	create_decoding_context_OA(dec_ctx, gc->meta.datasize, gc->meta.size_b, gc->meta.size_g, gc->meta.size_p, gc->meta.type, 0);
	while (dec_ctx->finished != 1) {
		struct coded_packet *pkt = generate_gnc_packet(gc);
		process_packet_OA(dec_ctx, pkt);
	}

	unsigned char *rec_buf = recover_data(dec_ctx->gc);
	if (memcmp(buf, rec_buf, datasize) != 0) 
		printf("recovered is NOT identical to original.\n");
	else
		printf("recovered is identical to original.\n");

	print_code_summary(&dec_ctx->gc->meta, dec_ctx->overhead, dec_ctx->operations);

	free_gnc_context(gc);
	free_decoding_context_OA(dec_ctx);
	return 0;
}