/**************************************************************
 * 		slncEncoder.c
 *
 * Functions for SLNC encoding. Coded packets can be generated
 * from memory buffer or files.
 **************************************************************/
#include "common.h"
#include "galois.h"
#include "bipartite.h"
#include "slncEncoder.h"
#include <math.h>
#include <sys/stat.h>
static int create_context_from_meta(struct slnc_context *sc);
static int verify_code_parameter(struct slnc_metainfo *meta);
static void perform_precoding(struct slnc_context *sc);
static int group_packets_rand(struct slnc_context *sc);
static int group_packets_band(struct slnc_context *sc);
static int group_packets_windwrap(struct slnc_context *sc);
static void encode_packet(struct slnc_context *sc, int gid, struct slnc_packet *pkt);
static int schedule_generation(struct slnc_context *sc);
/*
 * Create a GNC context containing meta information about the data to be encoded.
 *   buf      - Buffer containing bytes of data to be encoded
 *   datasize - size of data in bytes to be encoded
 *   sc       - pointer to the slnc_context where the context will be stored
 *   s_b	  - base generation size, size_b
 *   s_g	  - full generation size, size_g
 *   s_p	  - number of information symbols, size_p
 *
 * Return
 *   0  - Create successfully
 *   -1 - Create failed
 */
int slnc_create_enc_context(char *buf, long datasize, struct slnc_context **sc, struct slnc_parameter sp)
{
    static char fname[] = "slnc_create_enc_context";
    // Allocate file_context
    if ( (*sc = calloc(1, sizeof(struct slnc_context))) == NULL ) {
        fprintf(stderr, "%s: calloc file_context\n", fname);
        return(-1);
    }
    (*sc)->meta.pcrate   = sp.pcrate;	
    (*sc)->meta.size_b   = sp.size_b;
    (*sc)->meta.size_g   = sp.size_g;
    (*sc)->meta.size_p   = sp.size_p;
    (*sc)->meta.type     = sp.type;

    // Determine packet and generation numbers
    int num_src = ALIGN(datasize, (*sc)->meta.size_p);
    int num_chk = number_of_checks(num_src, (*sc)->meta.pcrate);
    (*sc)->meta.datasize = datasize;
    (*sc)->meta.snum  = num_src;							  // Number of source packets
    (*sc)->meta.cnum  = num_chk;							  // Number of check packets
    if ((*sc)->meta.type == BAND_SLNC) 
        (*sc)->meta.gnum  = ALIGN((num_src+num_chk-(*sc)->meta.size_g), (*sc)->meta.size_b) + 1; 
    else
        (*sc)->meta.gnum  = ALIGN( (num_src+num_chk), (*sc)->meta.size_b); 

    /*
     * Verify code parameter
     */
    if (verify_code_parameter(&((*sc)->meta)) != 0) {
        fprintf(stderr, "%s: code parameter is invalid.\n", fname);
        return(-1);
    }
    /*
     * Create generations, bipartite graph
     */
    if (create_context_from_meta(*sc) != 0) {
        fprintf(stderr, "%s: create_context_from_meta\n", fname);
        return(-1);
    }

    // Allocating pointers to data
    if (((*sc)->pp = calloc((*sc)->meta.snum+(*sc)->meta.cnum, sizeof(GF_ELEMENT*))) == NULL) {
        fprintf(stderr, "%s: calloc (*sc)->pp\n", fname);
        return(-1);
    }

    /*--------- Creating context with to-be-encoded data ---------------
     *
     * Currently we copy data from user-provided buffer. In the future,
     * we can make most of pp[i] directly point to user-provided buffer
     * address. Only the last source packet (due to zero-padding) and 
     * the cnum parity-check packets need to allocate new memory.
     *
     *------------------------------------------------------------------*/
    if (buf != NULL) {
        int alread = 0;
        for (int i=0; i<(*sc)->meta.snum+(*sc)->meta.cnum; i++) {
            (*sc)->pp[i] = calloc((*sc)->meta.size_p, sizeof(GF_ELEMENT));
            int toread = (alread+(*sc)->meta.size_p) <= (*sc)->meta.datasize ? (*sc)->meta.size_p : (*sc)->meta.datasize-alread;
            memcpy((*sc)->pp[i], buf+alread, toread*sizeof(GF_ELEMENT));
            alread += toread;
        }
        perform_precoding(*sc);
    }
    // Construct Galois field for encoding and decoding
    constructField(GF_POWER);

    return(0);
}


int slnc_create_enc_context_from_file(FILE *fp, struct slnc_context **sc, struct slnc_parameter sp)
{
    static char fname[] = "slnc_create_enc_context_from_file";
    // Get file size
    /* Seek to file end */
    if (fseek(fp, 0, SEEK_END) == -1) 
        fprintf(stderr, "%s: fseek SEEK_END\n", fname);
    long datasize = ftell(fp);			/* get file size */
    /* Seek back to file start */
    if (fseek(fp, 0, SEEK_SET) == -1) 
        fprintf(stderr, "%s: fseek SEEK_SET\n", fname);

    /* Create slnc_context without actual data */
    slnc_create_enc_context(NULL, datasize, sc, sp);
    return(0);
}

/*
 * Load file data into slnc_context created for the file. It's the
 * caller's responsibility to ensure that the pair of FILE* and 
 * slnc_context matches.
 */
int slnc_load_file_to_context(FILE *fp, struct slnc_context *sc)
{
    static char fname[] = "slnc_load_file_to_context";
    int alread = 0;
    for (int i=0; i<sc->meta.snum+sc->meta.cnum; i++) {
        sc->pp[i] = calloc(sc->meta.size_p, sizeof(GF_ELEMENT));
        int toread = (alread+sc->meta.size_p) <= sc->meta.datasize ? sc->meta.size_p : sc->meta.datasize-alread;
        if (fread(sc->pp[i], sizeof(GF_ELEMENT), toread, fp) != toread) {
            fprintf(stderr, "%s: fread sc->pp[%d]\n", fname, i);
            return (-1);
        }
        alread += toread;
    }
    perform_precoding(sc);
    return (0);
}

static int verify_code_parameter(struct slnc_metainfo *meta)
{
    if (meta->size_b > meta->size_g) {
        fprintf(stderr, "code spmeter error: size_b > size_g\n");
        return(-1);
    }
    if (meta->size_b*meta->size_p > meta->datasize) {
        fprintf(stderr, "code spmeter error: size_b X size_p > datasize\n");
        return(-1);
    }
    return(0);
}

/*
 * Create slnc context using metadata in fc
 */
static int create_context_from_meta(struct slnc_context *sc)
{
    static char fname[] = "slnc_create_enc_context_meta";
    // Inintialize generation structures
    sc->gene  = malloc(sizeof(struct subgeneration *) * sc->meta.gnum);
    if ( sc->gene == NULL ) {
        fprintf(stderr, "%s: malloc sc->gene\n", fname);
        return(-1);
    }
    for (int j=0; j<sc->meta.gnum; j++) {
        sc->gene[j] = malloc(sizeof(struct subgeneration));
        if ( sc->gene[j] == NULL ) { 
            fprintf(stderr, "%s: malloc sc->gene[%d]\n", fname, j);
            return(-1);
        }
        sc->gene[j]->gid = -1;
        sc->gene[j]->pktid = malloc(sizeof(int)*sc->meta.size_g);
        if ( sc->gene[j]->pktid == NULL ) {
            fprintf(stderr, "%s: malloc sc->gene[%d]->pktid\n", fname, j);
            return(-1);
        }
        memset(sc->gene[j]->pktid, -1, sizeof(int)*sc->meta.size_g);
    }

    int coverage;
    if (sc->meta.type == RAND_SLNC) 
        coverage = group_packets_rand(sc);
    else if (sc->meta.type == BAND_SLNC)
        coverage = group_packets_band(sc);
    else if (sc->meta.type == WINDWRAP_SLNC)
        coverage = group_packets_windwrap(sc);

#if defined(GNCTRACE)
    printf("Data Size: %ld\t Source Packets: %d\t Check Packets: %d\t Generations: %d\t Coverage: %d\n",sc->meta.datasize, sc->meta.snum, sc->meta.cnum,	sc->meta.gnum, coverage);
#endif
    // Creating bipartite graph of the precode
    if (sc->meta.cnum != 0) {
        if ( (sc->graph = malloc(sizeof(BP_graph))) == NULL ) {
            fprintf(stderr, "%s: malloc BP_graph\n", fname);
            return (-1);
        }
        create_bipartite_graph(sc->graph, sc->meta.snum, sc->meta.cnum);
    }
    return(0);
}

int slnc_free_enc_context(struct slnc_context *sc)
{
    int i;
    for (i=sc->meta.snum+sc->meta.cnum-1; i>=0; i--) {
        if (sc->pp[i] != NULL) {
            free(sc->pp[i]);
            sc->pp[i] = NULL;
        }
    }
    free(sc->pp);
    for (i=sc->meta.gnum-1; i>=0; i--) {
        free(sc->gene[i]->pktid);			// free packet IDs
        free(sc->gene[i]);					// free generation itself
        sc->gene[i] = NULL;
    }
    free(sc->gene);
    if (sc->graph != NULL)
        free_bipartite_graph(sc->graph);
    free(sc);
    return(0);
}

unsigned char *slnc_recover_data(struct slnc_context *sc)
{
    static char fname[] = "slnc_recover_data";
    long datasize = sc->meta.datasize;
    long alwrote = 0;
    long towrite = datasize;

    unsigned char *data;
    if ( (data = malloc(datasize)) == NULL) {
        fprintf(stderr, "%s: malloc(datasize) failed.\n", fname);
        return NULL;
    }
    int pc = 0;
    while (alwrote < datasize) {
        towrite = ((alwrote + sc->meta.size_p) <= datasize) ? sc->meta.size_p : datasize - alwrote;
        memcpy(data+alwrote, sc->pp[pc++], sizeof(GF_ELEMENT)*towrite);
        alwrote += towrite;
    }
    return data;
}

/* recover data to file */
long slnc_recover_data_to_file(FILE *fp, struct slnc_context *sc)
{
    static char fname[] = "slnc_recover_data";
    long datasize = sc->meta.datasize;
    long alwrote = 0;
    long towrite = datasize;

#if defined(GNCTRACE)
    printf("Writing to decoded file.\n");
#endif

    int pc = 0;
    while (alwrote < datasize) {
        towrite = ((alwrote + sc->meta.size_p) <= datasize) ? sc->meta.size_p : datasize - alwrote;
        if (fwrite(sc->pp[pc], sizeof(GF_ELEMENT), towrite, fp) != towrite) 
            fprintf(stderr, "%s: fwrite sc->pp[%d]\n", fname, pc);
        pc++;
        alwrote += towrite;
    }
    return alwrote;
}

// perform systematic LDPC precoding against SRC pkt list and results in a LDPC pkt list
static void perform_precoding(struct slnc_context *sc)
{
    static char fname[] = "perform_precoding";

    int i, j;
    for (i=0; i<sc->meta.cnum; i++) {
        // Encoding check packet according to the LDPC graph
        NBR_node *nb = sc->graph->l_nbrs_of_r[i]->first;
        while(nb != NULL) {
            int sid = nb->data;				// index of source packet
            // XOR information content
            galois_multiply_add_region(sc->pp[i+sc->meta.snum], sc->pp[sid], 1, sc->meta.size_p, GF_POWER);
            // move to next possible neighbour node of current check
            nb = nb->next;
        }
    }
}

/*
 * This routine uses a deterministic grouping scheme, so the need of sending
 * grouping information to clients is removed. The only information clients 
 * need to know is the number of packets, base size, and generation size. 
 */
static int group_packets_rand(struct slnc_context *sc)
{
    int num_p = sc->meta.snum + sc->meta.cnum;
    int num_g = sc->meta.gnum;

    int *selected = calloc(num_p, sizeof(int));

    int i, j;
    int index;
    int rotate = 0;
    for (i=0; i<num_g; i++) {
        sc->gene[i]->gid = i;
        // split packets into disjoint groups
        for (j=0; j<sc->meta.size_b; j++) {
            index = (i * sc->meta.size_b + j) % num_p;				// source packet index

            while (has_item(sc->gene[i]->pktid, index, j) != -1)
                index++;
            sc->gene[i]->pktid[j] = index;
            selected[index] += 1;
        }

        // fill in the rest of the generation with packets from other generations
        for (j=sc->meta.size_b; j<sc->meta.size_g; j++) {
            int tmp = i - (j - sc->meta.size_b + 7);
            int start = tmp >= 0 ? tmp : tmp+num_g;
            if (start == i)
                start++;
            index = (start * sc->meta.size_b + (j - sc->meta.size_b + rotate) % (sc->meta.size_g)) % num_p;
            while (has_item(sc->gene[i]->pktid, index, j) != -1)
                index++;
            sc->gene[i]->pktid[j] = index;
            selected[index] += 1;
        }
        rotate = (rotate + 7) % (sc->meta.size_g);
    }
    int coverage = 0;
    for (i=0; i<num_p; i++)
        coverage += selected[i];

    free(selected);
    return coverage;
}

/*
 * Group packets to generations that overlap head-to-toe. Each generation's
 * encoding coefficients form a band in GDM.
 */
static int group_packets_band(struct slnc_context *sc)
{
    int num_p = sc->meta.snum + sc->meta.cnum;
    int num_g = sc->meta.gnum;

    int *selected = calloc(num_p, sizeof(int));

    int i, j;
    int index;
    int leading_pivot = 0;
    for (i=0; i<num_g; i++) {
        sc->gene[i]->gid = i;
        leading_pivot = i * sc->meta.size_b;
        if (leading_pivot > num_p - sc->meta.size_g) {
#if defined(GNCTRACE)
            printf("Band lead of gid: %d is modified\n", i);
#endif
            leading_pivot = num_p - sc->meta.size_g;
        }
        for (j=0; j<sc->meta.size_g; j++) {
            index = leading_pivot + j;
            selected[index] += 1;
            sc->gene[i]->pktid[j] = index;
        }	
    }
    int coverage = 0;
    for (i=0; i<num_p; i++)
        coverage += selected[i];

    free(selected);
    return coverage;
}

/*
 * Group packets to generations that overlap consecutively. Wrap around if needed.
 */
static int group_packets_windwrap(struct slnc_context *sc)
{
    int num_p = sc->meta.snum + sc->meta.cnum;
    int num_g = sc->meta.gnum;

    int *selected = calloc(num_p, sizeof(int));

    int i, j;
    int index;
    int leading_pivot = 0;
    for (i=0; i<num_g; i++) {
        sc->gene[i]->gid = i;
        leading_pivot = i * sc->meta.size_b;
        for (j=0; j<sc->meta.size_g; j++) {
            index = (leading_pivot + j) % num_p;
            selected[index] += 1;
            sc->gene[i]->pktid[j] = index;
        }	
    }
    int coverage = 0;
    for (i=0; i<num_p; i++)
        coverage += selected[i];

    free(selected);
    return coverage;
}

/*
 * Allocate an empty GNC coded packet
 *  gid = -1
 *  coes: zeros
 *  syms: zeros
 */
struct slnc_packet *slnc_alloc_empty_packet(int size_g, int size_p)
{
    struct slnc_packet *pkt = calloc(1, sizeof(struct slnc_packet));
    if (pkt == NULL)
        return NULL;
    pkt->coes = calloc(size_g, sizeof(GF_ELEMENT));
    if (pkt->coes == NULL) 
        goto AllocErr;
    pkt->syms = calloc(size_p, sizeof(GF_ELEMENT));
    if (pkt->syms == NULL) 
        goto AllocErr;

    return pkt;

AllocErr:
    slnc_free_packet(pkt);
    return NULL;
}

/* Generate a GNC coded packet. Memory is allocated in the function. */
struct slnc_packet *slnc_generate_packet(struct slnc_context *sc)
{
    struct slnc_packet *pkt = slnc_alloc_empty_packet(sc->meta.size_g, sc->meta.size_p);
    int gid = schedule_generation(sc);
    encode_packet(sc, gid, pkt);
    return pkt;
}

/*
 * Generate a GNC coded packet in a given memory area.
 * It is the caller's responsibity to allocate memory properly.
 */
int slnc_generate_packet_im(struct slnc_context *sc, struct slnc_packet *pkt)
{
    if (pkt == NULL || pkt->coes == NULL || pkt->syms == NULL)
        return -1;
    memset(pkt->coes, 0, sc->meta.size_g*sizeof(GF_ELEMENT));
    memset(pkt->syms, 0, sc->meta.size_p*sizeof(GF_ELEMENT));
    int gid = schedule_generation(sc);
    encode_packet(sc, gid, pkt);
    return (0);
}

void slnc_free_packet(struct slnc_packet *pkt)
{
    if (pkt == NULL)
        return;
    if (pkt->coes != NULL)
        free(pkt->coes);
    if (pkt->syms != NULL)
        free(pkt->syms);
    free(pkt);
}


static void encode_packet(struct slnc_context *sc, int gid, struct slnc_packet *pkt)
{
    pkt->gid = gid;
    int i;
    GF_ELEMENT co;
    int pktid;
    for (i=0; i<sc->meta.size_g; i++) {
        pktid = sc->gene[gid]->pktid[i];							// The i-th packet of the gid-th generation
        co = (GF_ELEMENT) rand() % (1 << GF_POWER);					// Randomly generated coding coefficient
        galois_multiply_add_region(pkt->syms, sc->pp[pktid], co, sc->meta.size_p, GF_POWER);
        pkt->coes[i] = co;
    }
}

static int schedule_generation(struct slnc_context *sc)
{
    int gid = rand() % (sc->meta.gnum);
    return gid;
}

/*
 * Print code summary
 * If called by decoders, it prints overhead and operations as well.
 */
void print_code_summary(struct slnc_metainfo *meta, int overhead, long long operations)
{
    char typestr[20];
    switch(meta->type) {
        case RAND_SLNC:
            strcpy(typestr, "RAND");
            break;
        case BAND_SLNC:
            strcpy(typestr, "BAND");
            break;
        case WINDWRAP_SLNC:
            strcpy(typestr, "WINDWRAP");
            break;
        default:
            strcpy(typestr, "UNKNOWN");
    }
    printf("datasize: %d ", meta->datasize);
    printf("precode: %.3f ", meta->pcrate);
    printf("size_b: %d ", meta->size_b);
    printf("size_g: %d ", meta->size_g);
    printf("size_p: %d ", meta->size_p);
    printf("type: %s ", typestr);
    printf("snum: %d ", meta->snum);
    printf("cnum: %d ", meta->cnum);
    printf("gnum: %d ", meta->gnum);
    if (operations != 0) {
        printf("overhead: %.3f ", (double) overhead/meta->snum);
        printf("computation: %f\n", (double) operations/meta->snum/meta->size_p);
    }
}

