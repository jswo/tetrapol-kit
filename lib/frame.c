#define LOG_PREFIX "frame"
#include <tetrapol/log.h>
#include <tetrapol/tetrapol.h>
#include <tetrapol/frame.h>
#include <stdlib.h>
#include <string.h>

struct frame_decoder_priv_t {
    int band;
    int scr;
    int fr_type;
};

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

static void frame_descramble(uint8_t *fr_data_tmp, const uint8_t *fr_data,
        int scr)
{
    if (scr == 0) {
        memcpy(fr_data_tmp, fr_data, FRAME_DATA_LEN);
        return;
    }

    for(int k = 0 ; k < FRAME_DATA_LEN; k++) {
        fr_data_tmp[k] = fr_data[k] ^ scramb_table[(k + scr) % 127];
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

static void frame_diff_dec(uint8_t *fr_data)
{
    for (int j = FRAME_DATA_LEN - 1; j > 0; --j) {
        fr_data[j] ^= fr_data[j - diff_precod_UHF[j]];
    }
}

frame_decoder_t *frame_decoder_create(int band, int scr, int fr_type)
{
    frame_decoder_t *fd = malloc(sizeof(frame_decoder_t));
    if (!fd) {
        return NULL;
    }

    frame_decoder_reset(fd, band, scr, fr_type);

    return fd;
}

void frame_decoder_destroy(frame_decoder_t *fd)
{
    free(fd);
}

void frame_decoder_reset(frame_decoder_t *fd, int band, int scr, int fr_type)
{
    fd->band = band;
    fd->scr = scr;
    fd->fr_type = fr_type;
}

void frame_decoder_set_scr(frame_decoder_t *fd, int scr)
{
    fd->scr = scr;
}

void frame_decoder_decode(frame_decoder_t *fd, frame_t *fr, const uint8_t *fr_data)
{
    uint8_t fr_data_tmp[FRAME_DATA_LEN];

    frame_descramble(fr_data_tmp, fr_data, fd->scr);
    if (fd->band == TETRAPOL_BAND_UHF) {
        frame_diff_dec(fr_data_tmp);
    }
}
