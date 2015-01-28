#include "tetrapol.h"
#include "multiblock.h"
#include "tpdu.h"
#include "phys_ch.h"
#include "misc.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define DEBUG

// max error rate for 2 frame synchronization sequences
#define MAX_FRAME_SYNC_ERR 1

#define FRAME_HDR_LEN (8)
#define FRAME_DATA_LEN (152)
#define FRAME_LEN (FRAME_HDR_LEN + FRAME_DATA_LEN)

typedef struct {
    int frame_no;
    uint8_t data[FRAME_DATA_LEN];
} frame_t;

struct _tetrapol_phys_ch_t {
    int last_sync_err;  ///< errors in last frame synchronization sequence
    int total_sync_err; ///< cumulative error in framing
    int data_len;
    bool has_frame_sync;
    uint8_t data[10*FRAME_LEN];
};

int mod = -1;

/**
  PAS 0001-2 6.1.5.1
  PAS 0001-2 6.2.5.1
  PAS 0001-2 6.3.4.1

  Scrambling sequence was generated by this python3 script

  s = [1, 1, 1, 1, 1, 1, 1]
  for k in range(len(s), 127):
    s.append(s[k-1] ^ s[k-7])
  for i in range(len(s)):
    print(s[i], end=", ")
    if i % 8 == 7:
      print()
  */
static uint8_t scramb_table[127] = {
    1, 1, 1, 1, 1, 1, 1, 0,
    1, 0, 1, 0, 1, 0, 0, 1,
    1, 0, 0, 1, 1, 1, 0, 1,
    1, 1, 0, 1, 0, 0, 1, 0,
    1, 1, 0, 0, 0, 1, 1, 0,
    1, 1, 1, 1, 0, 1, 1, 0,
    1, 0, 1, 1, 0, 1, 1, 0,
    0, 1, 0, 0, 1, 0, 0, 0,
    1, 1, 1, 0, 0, 0, 0, 1,
    0, 1, 1, 1, 1, 1, 0, 0,
    1, 0, 1, 0, 1, 1, 1, 0,
    0, 1, 1, 0, 1, 0, 0, 0,
    1, 0, 0, 1, 1, 1, 1, 0,
    0, 0, 1, 0, 1, 0, 0, 0,
    0, 1, 1, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0,
};

static int process_frame(frame_t *frame);

void mod_set(int m) {
    mod=m;
}

tetrapol_phys_ch_t *tetrapol_phys_ch_create(void)
{
    tetrapol_phys_ch_t *t = malloc(sizeof(tetrapol_phys_ch_t));
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0, sizeof(tetrapol_phys_ch_t));

    return t;
}

void tetrapol_phys_ch_destroy(tetrapol_phys_ch_t *t)
{
    free(t);
}

static uint8_t differential_dec(uint8_t *data, int size, uint8_t last_bit)
{
    while (size--) {
        last_bit = *data = *data ^ last_bit;
        ++data;
    }
    return last_bit;
}

int tetrapol_recv2(tetrapol_phys_ch_t *t, uint8_t *buf, int len)
{
    const int space = sizeof(t->data) - t->data_len;
    len = (len > space) ? space : len;

    memcpy(t->data + t->data_len, buf, len);
    t->data_len += len;

    return len;
}

// compare bite stream to differentialy encoded synchronization sequence
static int cmp_frame_sync(const uint8_t *data)
{
    const uint8_t frame_dsync[] = { 1, 0, 1, 0, 0, 1, 1, };
    int sync_err = 0;
    for(int i = 0; i < sizeof(frame_dsync); ++i) {
        if (frame_dsync[i] != data[i + 1]) {
            ++sync_err;
        }
    }
    return sync_err;
}

/**
  Find 2 consecutive frame synchronization sequences.

  Using raw stream (before differential decoding) simplyfies search
  because only signal polarity must be considered,
  there is lot of troubles with error handlig after differential decoding.
  */
static int find_frame_sync(tetrapol_phys_ch_t *t)
{
    int offs = 0;
    int sync_err = MAX_FRAME_SYNC_ERR + 1;
    while (offs + FRAME_LEN + FRAME_HDR_LEN < t->data_len) {
        const uint8_t *data = t->data + offs;
        sync_err = cmp_frame_sync(data) +
            cmp_frame_sync(data + FRAME_LEN);
        if (sync_err <= MAX_FRAME_SYNC_ERR) {
            break;
        }

        ++offs;
    }

    t->data_len -= offs;
    memmove(t->data, t->data + offs, t->data_len);

    if (sync_err <= MAX_FRAME_SYNC_ERR) {
        t->last_sync_err = 0;
        t->total_sync_err = 0;
        return 1;
    }

    return 0;
}

/// return number of acquired frames (0 or 1) or -1 on error
static int get_frame(tetrapol_phys_ch_t *t, frame_t *frame)
{
    if (t->data_len < FRAME_LEN) {
        return 0;
    }
    const int sync_err = cmp_frame_sync(t->data);
    if (sync_err + t->last_sync_err > MAX_FRAME_SYNC_ERR) {
        t->total_sync_err = 1 + 2 * t->total_sync_err;
        if (t->total_sync_err >= FRAME_LEN) {
            return -1;
        }
    } else {
        t->total_sync_err = 0;
    }

    t->last_sync_err = sync_err;
    memcpy(frame->data, t->data + FRAME_HDR_LEN, FRAME_DATA_LEN);
    differential_dec(frame->data, FRAME_DATA_LEN, 0);
    t->data_len -= FRAME_LEN;
    memmove(t->data, t->data + FRAME_LEN, t->data_len);

    return 1;
}

int tetrapol_phys_ch_process(tetrapol_phys_ch_t *t)
{
    if (!t->has_frame_sync) {
        t->has_frame_sync = find_frame_sync(t);
        if (!t->has_frame_sync) {
            return 0;
        }
        fprintf(stderr, "Frame sync found\n");
        multiblock_reset();
        segmentation_reset();
    }

    int r = 1;
    frame_t frame;
    while ((r = get_frame(t, &frame)) > 0) {
        process_frame(&frame);
    }

    if (r == 0) {
        return 0;
    }

    fprintf(stderr, "Frame sync lost\n");
    mod = -1;
    t->has_frame_sync = false;

    return 0;
}

// http://ghsi.de/CRC/index.php?Polynom=10010
static void mk_crc5(uint8_t *res, const uint8_t *input, int input_len)
{
    uint8_t inv;
    memset(res, 0, 5);

    for (int i = 0; i < input_len; ++i)
    {
        inv = input[i] ^ res[0];         // XOR required?

        res[0] = res[1];
        res[1] = res[2];
        res[2] = res[3] ^ inv;
        res[3] = res[4];
        res[4] = inv;
    }
}

static int check_data_crc(const uint8_t *d)
{
    uint8_t crc[5];
    int res;

    mk_crc5(crc, d, 69);
    res = memcmp(d+69, crc, 5);
    //	printf("crc=");
    //	print_buf(d+69,5);
    //	printf("crcc=");
    //	print_buf(crc,5);
    return res ? 0 : 1;
}

static void decode_data_frame(const frame_t *f, uint8_t *d)
{
    // decode first 52 bites of frame
    uint8_t b1[26];

    int check = 1;

    memset(b1, 2, 26);
    memset(d, 2, 74);

    // b'(25) = b'(-1)
    // b'(j-1) = C(2j) - C(2j+1)

    // j=0
    b1[25] = f->data[0] ^ f->data[1];
    for(int j = 1; j <= 25; j++) {
        b1[j-1] = f->data[2*j] ^ f->data[2*j+1];
    }

    //	printf("b1=");
    //	print_buf(b1,26);

    for(int j = 0; j <= 25; j++)
        d[j]=b1[j];

    if ((f->data[150] != f->data[151]) ||
            ((f->data[148] ^ f->data[149]) != f->data[150]) ||
            (f->data[52] != f->data[53])) {
        check=0;
    }

    for (int j = 3; j < 23; j++) {
        if (f->data[2*j] != (b1[j] ^ b1[j-1] ^ b1[j-2]))
            check=0;
    }

    // TODO: check frame type (AUDIO / DATA)
    // decode remaining part of frame
    uint8_t b2[50];
    memset(b2, 2, 50);
    b2[0] = f->data[53];
    for(int j = 2; j <= 48; j++) {
        b2[j-1] = f->data[2*j+52] ^ f->data[2*j+53];
    }

    //	printf("b2=");
    //	print_buf(b2,48);

    for(int j = 0; j <= 47; j++) {
        d[j+26] = b2[j];
    }
    for (int j = 3; j < 45; j++) {
        if (f->data[2*j+52] != (b2[j] ^ b2[j-1] ^ b2[j-2]))
            check=0;
    }

    if (!check) {
        d[0]=2;
    }
}

// PAS 0001-2 6.1.4.1
static const int interleave_voice_UHF[] = {
    1, 77, 38, 114, 20, 96, 59, 135,
    3, 79, 41, 117, 23, 99, 62, 138,
    5, 81, 44, 120, 26, 102, 65, 141,
    8, 84, 47, 123, 29, 105, 68, 144,
    11, 87, 50, 126, 32, 108, 71, 147,
    14, 90, 53, 129, 35, 111, 74, 150,
    17, 93, 56, 132, 37, 113, 73, 4,
    0, 76, 40, 119, 19, 95, 58, 137,
    151, 80, 42, 115, 24, 100, 60, 133,
    12, 88, 48, 121, 30, 106, 66, 139,
    18, 91, 51, 124, 28, 104, 67, 146,
    10, 89, 52, 131, 34, 110, 70, 149,
    13, 97, 57, 130, 36, 112, 75, 148,
    6, 82, 39, 116, 16, 92, 55, 134,
    2, 78, 43, 122, 22, 98, 61, 140,
    9, 85, 45, 118, 27, 103, 63, 136,
    15, 83, 46, 125, 25, 101, 64, 143,
    7, 86, 49, 128, 31, 107, 69, 142,
    21, 94, 54, 127, 33, 109, 72, 145,
};

// PAS 0001-2 6.2.4.1
static const int interleave_data_UHF[] = {
    1, 77, 38, 114, 20, 96, 59, 135,
    3, 79, 41, 117, 23, 99, 62, 138,
    5, 81, 44, 120, 26, 102, 65, 141,
    8, 84, 47, 123, 29, 105, 68, 144,
    11, 87, 50, 126, 32, 108, 71, 147,
    14, 90, 53, 129, 35, 111, 74, 150,
    17, 93, 56, 132, 37, 112, 76, 148,
    2, 88, 40, 115, 19, 97, 58, 133,
    4, 75, 43, 118, 22, 100, 61, 136,
    7, 85, 46, 121, 25, 103, 64, 139,
    10, 82, 49, 124, 28, 106, 67, 142,
    13, 91, 52, 127, 31, 109, 73, 145,
    16, 94, 55, 130, 34, 113, 70, 151,
    0, 80, 39, 116, 21, 95, 57, 134,
    6, 78, 42, 119, 24, 98, 60, 137,
    9, 83, 45, 122, 27, 101, 63, 140,
    12, 86, 48, 125, 30, 104, 66, 143,
    15, 89, 51, 128, 33, 107, 69, 146,
    18, 92, 54, 131, 36, 110, 72, 149,
};

static void frame_deinterleave(frame_t *f)
{
    uint8_t tmp[FRAME_DATA_LEN];
    memcpy(tmp, f->data, FRAME_DATA_LEN);

    for (int j = 0; j < FRAME_DATA_LEN; ++j) {
        f->data[j] = tmp[interleave_data_UHF[j]];
    }
}


/**
  PAS 0001-2 6.1.4.2
  PAS 0001-2 6.2.4.2

  Audio and data frame differencial precoding index table was generated by the
  following python 3 scipt.

  pre_cod = ( 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40,
             43, 46, 49, 52, 55, 58, 61, 64, 67, 70, 73, 76,
             83, 86, 89, 92, 95, 98, 101, 104, 107, 110, 113, 116,
            119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149 )
  for i in range(152):
      print(1+ (i in pre_cod), end=", ")
      if i % 8 == 7:
          print()
*/
static const int diff_precod_UHF[] = {
    1, 1, 1, 1, 1, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 1,
    1, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
    2, 1, 1, 2, 1, 1, 2, 1,
    1, 2, 1, 1, 2, 1, 1, 2,
    1, 1, 2, 1, 1, 2, 1, 1,
};

static void frame_diff_dec(frame_t *f)
{
    for (int j = FRAME_DATA_LEN - 1; j > 0; --j) {
        f->data[j] ^= f->data[j - diff_precod_UHF[j]];
    }
}

static void frame_descramble(frame_t *f, int scr)
{
    if (scr == 0) {
        return;
    }

    for(int k = 0 ; k < FRAME_DATA_LEN; k++) {
        f->data[k] ^= scramb_table[(k + scr) % 127];
    }
}

#define FRAME_BITORDER_LEN 64

static void bitorder_frame(const uint8_t *d, uint8_t *out)
{
    for (int i = 0; i < 8; i++) {
        for(int j = 0; j < 8; j++) {
            out[8*i + j] =  d[i*8 + 7-j];
        }
    }
}

static int process_frame(frame_t *f)
{
    int scr, scr2, i, j;
    uint8_t asbx, asby, fn0, fn1;
    uint8_t frame_bord[FRAME_BITORDER_LEN];

    if (mod != -1)
        mod++;
    if (mod==200)
        mod=0;

    //	printf("s=");
    //	print_buf(scramb_table,127);
    //	printf("f=");
    //	print_buf(f,160);

    //	printf("Attempting descramble\n");
    int scr_ok=0;
    for(scr=0; scr<=127; scr++) {
        //		printf("trying scrambling %i\n", scr);

        frame_t f_;
        memcpy(&f_, f, sizeof(f_));

        frame_descramble(&f_, scr);
        frame_diff_dec(&f_);
        frame_deinterleave(&f_);

        uint8_t d[FRAME_DATA_LEN];
        decode_data_frame(&f_, d);
        //		printf("d=");
        //		print_buf(d,74);

        if(d[0]!=1) {
            //			printf("not data frame!\n");
            continue;
        }

        if(!check_data_crc(d)) {
            //			printf("crc mismatch!\n");
            continue;
        }
        //		printf("b=");
        //		print_buf(d+1, 68);

        scr2=scr;
        asbx=d[67];			// maybe x=68, y=67
        asby=d[68];
        fn0=d[2];
        fn1=d[1];
        bitorder_frame(d+3, frame_bord);

        scr_ok++;
    }
    if(scr_ok==1) {
        printf("OK mod=%03i fn=%i%i asb=%i%i scr=%03i ", mod, fn0, fn1, asbx, asby, scr2);
        for (i=0; i<8; i++) {
            for(j=0; j<8; j++)
                printf("%i", frame_bord[i*8+j]);
            printf(" ");
        }
        print_buf(frame_bord, 64);
        multiblock_process(frame_bord, 2*fn0 + fn1, mod);
    } else {
        printf("ERR2 mod=%03i\n", mod);
        multiblock_reset();
        segmentation_reset();
    }

    return 0;
}
