#include <string.h>
#include <unistd.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>
#include "bam_endian.h"
#endif
#include "faidx.h"
#include "sam.h"

#define TYPE_BAM    1
#define TYPE_READ   2
#define TYPE_MT     0x10

bam_header_t *bam_header_dup(const bam_header_t *h0)
{
	bam_header_t *h;
	int i;
	h = bam_header_init();
	*h = *h0;
	h->hash = h->dict = h->rg2lib = 0;
	h->text = (char*)calloc(h->l_text + 1, 1);
	memcpy(h->text, h0->text, h->l_text);
	h->target_len = (uint32_t*)calloc(h->n_targets, 4);
	h->target_name = (char**)calloc(h->n_targets, sizeof(void*));
	for (i = 0; i < h->n_targets; ++i) {
		h->target_len[i] = h0->target_len[i];
		h->target_name[i] = strdup(h0->target_name[i]);
	}
	return h;
}
static void append_header_text(bam_header_t *header, char* text, int len)
{
	int x = header->l_text + 1;
	int y = header->l_text + len + 1; // 1 byte null
	if (text == 0) return;
	kroundup32(x); 
	kroundup32(y);
	if (x < y) header->text = (char*)realloc(header->text, y);
	strncpy(header->text + header->l_text, text, len); // we cannot use strcpy() here.
	header->l_text += len;
	header->text[header->l_text] = 0;
}

#ifdef HAVE_PTHREAD
static inline int sam_write1_core(FILE *fp, const bam1_core_t *c, int data_len, uint8_t *data)
{
	void bam_swap_endian_data(const bam1_core_t *c, int data_len, uint8_t *data);
	uint32_t x[8], block_len = data_len + BAM_CORE_SIZE, y;
	int i;
	x[0] = c->tid;
	x[1] = c->pos;
	x[2] = (uint32_t)c->bin<<16 | c->qual<<8 | c->l_qname;
	x[3] = (uint32_t)c->flag<<16 | c->n_cigar;
	x[4] = c->l_qseq;
	x[5] = c->mtid;
	x[6] = c->mpos;
	x[7] = c->isize;
	if (bam_is_be) {
		for (i = 0; i < 8; ++i) bam_swap_endian_4p(x + i);
		y = block_len;
		fwrite(bam_swap_endian_4p(&y), 4, 1, fp);
		bam_swap_endian_data(c, data_len, data);
	} else fwrite(&block_len, 4, 1, fp);
	fwrite(x, 4, 8, fp);
	fwrite(data, data_len, 1, fp);
	if (bam_is_be) bam_swap_endian_data(c, data_len, data);
	return 4 + block_len;
}
static void *worker(void *data)
{
	samfile_t *fp = (samfile_t*)data;
	int l;
	while ((l = fread(fp->buf, 1, 0x10000, fp->fp[0])) > 0)
		bgzf_write(fp->x.bam, fp->buf, l);
	return 0;
}
#endif

samfile_t *samopen(const char *fn, const char *mode, const void *aux)
{
	samfile_t *fp;
	fp = (samfile_t*)calloc(1, sizeof(samfile_t));
	if (strchr(mode, 'r')) { // read
		fp->type |= TYPE_READ;
		if (strchr(mode, 'b')) { // binary
			fp->type |= TYPE_BAM;
			fp->x.bam = strcmp(fn, "-")? bam_open(fn, "r") : bam_dopen(fileno(stdin), "r");
			if (fp->x.bam == 0) goto open_err_ret;
			fp->header = bam_header_read(fp->x.bam);
		} else { // text
			fp->x.tamr = sam_open(fn);
			if (fp->x.tamr == 0) goto open_err_ret;
			fp->header = sam_header_read(fp->x.tamr);
			if (fp->header->n_targets == 0) { // no @SQ fields
				if (aux) { // check if aux is present
					bam_header_t *textheader = fp->header;
					fp->header = sam_header_read2((const char*)aux);
					if (fp->header == 0) goto open_err_ret;
					append_header_text(fp->header, textheader->text, textheader->l_text);
					bam_header_destroy(textheader);
				}
				if (fp->header->n_targets == 0)
					fprintf(stderr, "[samopen] no @SQ lines in the header.\n");
			} else fprintf(stderr, "[samopen] SAM header is present: %d sequences.\n", fp->header->n_targets);
		}
	} else if (strchr(mode, 'w')) { // write
		fp->header = bam_header_dup((const bam_header_t*)aux);
		if (strchr(mode, 'b')) { // binary
			char bmode[3];
			bmode[0] = 'w'; bmode[1] = strstr(mode, "u")? 'u' : 0; bmode[2] = 0;
			fp->type |= TYPE_BAM;
			fp->x.bam = strcmp(fn, "-")? bam_open(fn, bmode) : bam_dopen(fileno(stdout), bmode);
			if (fp->x.bam == 0) goto open_err_ret;
			bam_header_write(fp->x.bam, fp->header);
#ifdef HAVE_PTHREAD
			if (strchr(mode, 't')) fp->type |= TYPE_MT;
			if (fp->type & TYPE_MT) {
				int fd[2];
				pthread_attr_t attr;
				pipe(fd);
				fp->fp[0] = fdopen(fd[0], "rb");
				fp->fp[1] = fdopen(fd[1], "wb");
				pthread_attr_init(&attr);
				pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
				pthread_create(&fp->tid, &attr, worker, (void*)fp);
			}
#endif
		} else { // text
			// open file
			fp->x.tamw = strcmp(fn, "-")? fopen(fn, "w") : stdout;
			if (fp->x.tamr == 0) goto open_err_ret;
			if (strstr(mode, "X")) fp->type |= BAM_OFSTR<<2;
			else if (strstr(mode, "x")) fp->type |= BAM_OFHEX<<2;
			else fp->type |= BAM_OFDEC<<2;
			// write header
			if (strchr(mode, 'h')) {
				int i;
				bam_header_t *alt;
				// parse the header text 
				alt = bam_header_init();
				alt->l_text = fp->header->l_text; alt->text = fp->header->text;
				sam_header_parse(alt);
				alt->l_text = 0; alt->text = 0;
				// check if there are @SQ lines in the header
				fwrite(fp->header->text, 1, fp->header->l_text, fp->x.tamw);
				if (alt->n_targets) { // then write the header text without dumping ->target_{name,len}
					if (alt->n_targets != fp->header->n_targets)
						fprintf(stderr, "[samopen] inconsistent number of target sequences.\n");
				} else { // then dump ->target_{name,len}
					for (i = 0; i < fp->header->n_targets; ++i)
						fprintf(fp->x.tamw, "@SQ\tSN:%s\tLN:%d\n", fp->header->target_name[i], fp->header->target_len[i]);
				}
				bam_header_destroy(alt);
			}
		}
	}
	return fp;

open_err_ret:
	free(fp);
	return 0;
}

void samclose(samfile_t *fp)
{
	if (fp == 0) return;
	if (fp->type & TYPE_BAM) {
#ifdef HAVE_PTHREAD
		if (!(fp->type&TYPE_READ) && (fp->type&TYPE_MT)) {
			fclose(fp->fp[1]);
			pthread_join(fp->tid, 0);
			fclose(fp->fp[0]);
		}
#endif
		bam_close(fp->x.bam);
	} else if (fp->type & TYPE_READ) sam_close(fp->x.tamr);
	if (fp->header) bam_header_destroy(fp->header);
	else fclose(fp->x.tamw);
	free(fp);
}

int samread(samfile_t *fp, bam1_t *b)
{
	if (fp == 0 || !(fp->type & TYPE_READ)) return -1; // not open for reading
	if (fp->type & TYPE_BAM) return bam_read1(fp->x.bam, b);
	else return sam_read1(fp->x.tamr, fp->header, b);
}

int samwrite(samfile_t *fp, const bam1_t *b)
{
	if (fp == 0 || (fp->type & TYPE_READ)) return -1; // not open for writing
	if (fp->type & TYPE_BAM) {
		if (fp->type & TYPE_MT) {
#ifdef HAVE_PTHREAD
			return sam_write1_core(fp->fp[1], &b->core, b->data_len, b->data);
#else
			return bam_write1(fp->x.bam, b);
#endif
		} else return bam_write1(fp->x.bam, b);
	} else {
		char *s = bam_format1_core(fp->header, b, fp->type>>2&3);
		int l = strlen(s);
		fputs(s, fp->x.tamw); fputc('\n', fp->x.tamw);
		free(s);
		return l + 1;
	}
}

int sampileup(samfile_t *fp, int mask, bam_pileup_f func, void *func_data)
{
	bam_plbuf_t *buf;
	int ret;
	bam1_t *b;
	b = bam_init1();
	buf = bam_plbuf_init(func, func_data);
	bam_plbuf_set_mask(buf, mask);
	while ((ret = samread(fp, b)) >= 0)
		bam_plbuf_push(b, buf);
	bam_plbuf_push(0, buf);
	bam_plbuf_destroy(buf);
	bam_destroy1(b);
	return 0;
}

char *samfaipath(const char *fn_ref)
{
	char *fn_list = 0;
	if (fn_ref == 0) return 0;
	fn_list = calloc(strlen(fn_ref) + 5, 1);
	strcat(strcpy(fn_list, fn_ref), ".fai");
	if (access(fn_list, R_OK) == -1) { // fn_list is unreadable
		if (access(fn_ref, R_OK) == -1) {
			fprintf(stderr, "[samfaipath] fail to read file %s.\n", fn_ref);
		} else {
			fprintf(stderr, "[samfaipath] build FASTA index...\n");
			if (fai_build(fn_ref) == -1) {
				fprintf(stderr, "[samfaipath] fail to build FASTA index.\n");
				free(fn_list); fn_list = 0;
			}
		}
	}
	return fn_list;
}
